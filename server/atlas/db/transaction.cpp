#include "atlas/db/transaction.h"

#include <exception>
#include <utility>

#include "atlas/core/error.h"
#include "atlas/core/log.h"

namespace atlas {

Transaction::Transaction(Connection& connection, Ctx& ctx) : connection_(&connection), ctx_(&ctx) {
    connection_->Begin();
    open_ = true;
    ctx_->tx_state = TxState::Active;
}

Transaction::~Transaction() {
    if (!open_) {
        return;
    }
    // 🔴 Reached while an exception is in flight in the case this class exists for, so nothing here
    // may throw. Connection::Rollback is noexcept and logs its own failure.
    open_ = false;
    connection_->Rollback();
    ctx_->tx_state = TxState::RolledBack;
}

void Transaction::Commit() {
    ATLAS_CHECK(open_, "transaction commit on a scope that is not open");
    // Cleared BEFORE the call: if Commit throws, the server has already ended the transaction and a
    // rollback from the destructor would be issued against a connection that is no longer in one.
    open_ = false;
    connection_->Commit();
    ctx_->tx_state = TxState::Committed;
}

PostCommitGuard::PostCommitGuard(Compensation compensation)
    : compensation_(std::move(compensation)) {}

PostCommitGuard::~PostCommitGuard() {
    if (!armed_ || !compensation_) {
        return;
    }
    armed_ = false;
    try {
        compensation_();
    } catch (const std::exception& ex) {
        ATLAS_LOG_ERROR("post-commit compensation failed; committed state is inconsistent: {}",
                        ex.what());
    } catch (...) {
        ATLAS_LOG_ERROR(
            "post-commit compensation failed with a non-std exception; committed state is "
            "inconsistent");
    }
}

}  // namespace atlas
