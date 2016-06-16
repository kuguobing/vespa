// Copyright 2016 Yahoo Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
#include <vespa/fastos/fastos.h>
#include "feedstates.h"
#include "feedconfigstore.h"
#include "ireplaypackethandler.h"
#include "replaypacketdispatcher.h"
#include <vespa/searchcore/proton/common/eventlogger.h>
#include <vespa/vespalib/util/closuretask.h>
#include <vespa/searchcore/proton/bucketdb/ibucketdbhandler.h>
#include <vespa/log/log.h>
LOG_SETUP(".proton.server.feedstates");

using search::transactionlog::Packet;
using search::transactionlog::RPC;
using search::SerialNum;
using vespalib::Executor;
using vespalib::IllegalStateException;
using vespalib::makeClosure;
using vespalib::makeTask;
using vespalib::make_string;
using proton::bucketdb::IBucketDBHandler;

namespace proton {

namespace {
typedef vespalib::Closure1<const Packet::Entry &>::UP EntryHandler;

const search::SerialNum REPLAY_PROGRESS_INTERVAL = 50000;

void
handleProgress(TlsReplayProgress &progress, SerialNum currentSerial)
{
    progress.updateCurrent(currentSerial);
    if (LOG_WOULD_LOG(event) && (LOG_WOULD_LOG(debug) ||
            (progress.getCurrent() % REPLAY_PROGRESS_INTERVAL == 0)))
    {
        EventLogger::transactionLogReplayProgress(progress.getDomainName(),
                                                  progress.getProgress(),
                                                  progress.getFirst(),
                                                  progress.getLast(),
                                                  progress.getCurrent());
    }
}

void
handlePacket(PacketWrapper::SP wrap, EntryHandler entryHandler)
{
    vespalib::nbostream handle(wrap->packet.getHandle().c_str(),
                               wrap->packet.getHandle().size(),
                               true);
    while (handle.size() > 0) {
        Packet::Entry entry;
        entry.deserialize(handle);
        entryHandler->call(entry);
        if (wrap->progress != NULL) {
            handleProgress(*wrap->progress, entry.serial());
        }
    }
    wrap->result = RPC::OK;
    wrap->gate.countDown();
}

class TransactionLogReplayPacketHandler : public IReplayPacketHandler {
    IFeedView *& _feed_view_ptr;  // Pointer can be changed in executor thread.
    IBucketDBHandler &_bucketDBHandler;
    IReplayConfig &_replay_config;
    FeedConfigStore &_config_store;

    void handleTransactionLogEntry(const Packet::Entry &entry);

public:
    TransactionLogReplayPacketHandler(IFeedView *& feed_view_ptr,
                                      IBucketDBHandler &bucketDBHandler,
                                      IReplayConfig &replay_config,
                                      FeedConfigStore &config_store)
        : _feed_view_ptr(feed_view_ptr),
          _bucketDBHandler(bucketDBHandler),
          _replay_config(replay_config),
          _config_store(config_store) {
    }

    virtual void replay(const PutOperation &op) {
        _feed_view_ptr->handlePut(NULL, op);
    }
    virtual void replay(const RemoveOperation &op) {
        _feed_view_ptr->handleRemove(NULL, op);
    }
    virtual void replay(const UpdateOperation &op) {
        _feed_view_ptr->handleUpdate(NULL, op);
    }
    virtual void replay(const NoopOperation &) {} // ignored
    virtual void replay(const NewConfigOperation &op) {
        _replay_config.replayConfig(op.getSerialNum());
    }
    virtual void replay(const WipeHistoryOperation &op) {
        _config_store.saveWipeHistoryConfig(op.getSerialNum(),
                                            op.getWipeTimeLimit());
        _replay_config.replayWipeHistory(op.getSerialNum(),
                                         op.getWipeTimeLimit());
    }
    virtual void replay(const DeleteBucketOperation &op) {
        _feed_view_ptr->handleDeleteBucket(op);
    }
    virtual void replay(const SplitBucketOperation &op) {
        _bucketDBHandler.handleSplit(op.getSerialNum(),
                                     op.getSource(),
                                     op.getTarget1(),
                                     op.getTarget2());
    }
    virtual void replay(const JoinBucketsOperation &op) {
        _bucketDBHandler.handleJoin(op.getSerialNum(),
                                    op.getSource1(),
                                    op.getSource2(),
                                    op.getTarget());
    }
    virtual void replay(const PruneRemovedDocumentsOperation &op) {
        _feed_view_ptr->handlePruneRemovedDocuments(op);
    }
    virtual void replay(const SpoolerReplayStartOperation &op) {
        (void) op;
    }
    virtual void replay(const SpoolerReplayCompleteOperation &op) {
        (void) op;
    }
    virtual void replay(const MoveOperation &op) {
        _feed_view_ptr->handleMove(op);
    }
    virtual void replay(const CreateBucketOperation &) {
    }
    virtual void replay(const CompactLidSpaceOperation &op) {
        _feed_view_ptr->handleCompactLidSpace(op);
    }
    virtual NewConfigOperation::IStreamHandler &getNewConfigStreamHandler() {
        return _config_store;
    }
    virtual document::DocumentTypeRepo &getDeserializeRepo() {
        return *_feed_view_ptr->getDocumentTypeRepo();
    }
};

void startDispatch(IReplayPacketHandler *packet_handler,
                   const Packet::Entry &entry) {
    // Called by handlePacket() in executor thread.
    LOG(spam,
        "replay packet entry: entrySerial(%" PRIu64 "), entryType(%u)",
        entry.serial(), entry.type());

    ReplayPacketDispatcher dispatcher(*packet_handler);
    dispatcher.replayEntry(entry);
}

}  // namespace

ReplayTransactionLogState::ReplayTransactionLogState(
        const vespalib::string &name,
        IFeedView *& feed_view_ptr,
        IBucketDBHandler &bucketDBHandler,
        IReplayConfig &replay_config,
        FeedConfigStore &config_store)
    : FeedState(REPLAY_TRANSACTION_LOG),
      _doc_type_name(name),
      _packet_handler(new TransactionLogReplayPacketHandler(
                      feed_view_ptr, bucketDBHandler,
                      replay_config, config_store)) {
}

void ReplayTransactionLogState::receive(const PacketWrapper::SP &wrap,
                                        Executor &executor) {
    EntryHandler closure = makeClosure(&startDispatch, _packet_handler.get());
    executor.execute(makeTask(makeClosure(&handlePacket, wrap, std::move(closure))));
}

}  // namespace proton
