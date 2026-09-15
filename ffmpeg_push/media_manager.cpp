#include "media_manager.h"

#include "reader_pool.h"

MediaManager& MediaManager::instance() {
    static MediaManager inst;
    return inst;
}

void MediaManager::setThreads(int reader_threads, int poller_threads) {
    if (!reader_pool_)
        reader_pool_ = std::make_unique<ReaderPool>(reader_threads);
    if (!poller_pool_)
        poller_pool_ = std::make_unique<kit::EventPollerPool>(poller_threads);
}

MediaSource::Ptr MediaManager::createMedia(const std::string& app,
                                           const std::string& stream) {
    if (!poller_pool_)
        poller_pool_ = std::make_unique<kit::EventPollerPool>(4);
    auto m = MediaSource::create(app, stream, poller_pool_->getPoller());
    m->setReconnectPool(&reconnect_pool_);
    {
        std::lock_guard<std::mutex> lk(sources_mtx_);
        sources_.emplace_back(m);
    }
    return m;
}

bool MediaManager::bindFile(const MediaSource::Ptr& m,
                            const std::string& file) {
    if (!reader_pool_)
        reader_pool_ = std::make_unique<ReaderPool>(4);
    return reader_pool_->addSource(m, file);
}

bool MediaManager::startSendRtsp(const MediaSource::Ptr& m,
                                 const std::string& url) {
    return m->startSendRtsp(url);
}

void MediaManager::shutdown() {
    // Stop every live MediaSource and wait for each stop to run on its
    // poller thread: a reconnect task only leaves its retry loop once
    // Session::stopped is set, so without this the reconnect pool's
    // destructor join could hang forever against a silent server.
    kit::semaphore done;
    int pending = 0;
    {
        std::lock_guard<std::mutex> lk(sources_mtx_);
        for (auto& w : sources_) {
            auto m = w.lock();
            if (!m) continue;
            m->stop(&done);
            ++pending;
        }
    }
    for (int i = 0; i < pending; ++i)
        done.wait();

    if (reader_pool_) reader_pool_->stop();
}
