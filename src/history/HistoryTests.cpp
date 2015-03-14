// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC
#include "util/asio.h"
#include "main/Application.h"
#include "history/HistoryMaster.h"
#include "history/HistoryArchive.h"
#include "main/test.h"
#include "main/Config.h"
#include "clf/CLFMaster.h"
#include "clf/BucketList.h"
#include "crypto/Hex.h"
#include "lib/catch.hpp"
#include "util/Fs.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/TmpDir.h"
#include "transactions/TxTests.h"
#include "ledger/LedgerMaster.h"
#include <cstdio>
#include <xdrpp/autocheck.h>
#include <fstream>
#include <random>

using namespace stellar;

namespace stellar {
using xdr::operator==;
};

Config&
addLocalDirHistoryArchive(TmpDir const& dir, Config &cfg, bool writable=true)
{
    std::string d = dir.getName();
    std::string getCmd = "cp " + d + "/{0} {1}";
    std::string putCmd = "";
    std::string mkdirCmd = "";

    if (writable)
    {
        putCmd = "cp {0} " + d + "/{1}";
        mkdirCmd = "mkdir -p " + d + "/{0}";
    }

    cfg.HISTORY["test"] =
        std::make_shared<HistoryArchive>("test", getCmd, putCmd, mkdirCmd);
    return cfg;
}


class HistoryTests
{
protected:

    VirtualClock clock;
    TmpDirMaster archtmp;
    TmpDir dir;
    Config cfg;
    std::vector<Config> mCfgs;
    Application::pointer appPtr;
    Application &app;
    std::default_random_engine mGenerator;
    std::bernoulli_distribution mFlip{0.5};

    std::vector<uint32_t> mLedgerSeqs;
    std::vector<uint256> mLedgerHashes;
    std::vector<uint256> mBucket0Hashes;
    std::vector<uint256> mBucket1Hashes;

public:
    HistoryTests()
        : archtmp("archtmp")
        , dir(archtmp.tmpDir("archive"))
        , cfg(getTestConfig())
        , appPtr(Application::create(clock, addLocalDirHistoryArchive(dir, cfg)))
        , app(*appPtr)
        {
            CHECK(HistoryMaster::initializeHistoryArchive(app, "test"));
        }

    void crankTillDone(bool& done);
    void generateAndPublishHistory(size_t nPublishes);
    Application::pointer catchupNewApplication(uint32_t lastLedger,
                                               uint32_t initLedger,
                                               Config::TestDbMode dbMode,
                                               HistoryMaster::ResumeMode resumeMode,
                                               std::string const& appName,
                                               Application::pointer app2 = nullptr);

    bool flip() { return mFlip(mGenerator); }
};

void
HistoryTests::crankTillDone(bool& done)
{
    while (!done && !app.getClock().getIOService().stopped())
    {
        app.getClock().crank(false);
    }
}

TEST_CASE("next checkpoint ledger", "[history]")
{
    CHECK(HistoryMaster::nextCheckpointLedger(0) == 64);
    CHECK(HistoryMaster::nextCheckpointLedger(1) == 64);
    CHECK(HistoryMaster::nextCheckpointLedger(32) == 64);
    CHECK(HistoryMaster::nextCheckpointLedger(62) == 64);
    CHECK(HistoryMaster::nextCheckpointLedger(63) == 64);
    CHECK(HistoryMaster::nextCheckpointLedger(64) == 64);
    CHECK(HistoryMaster::nextCheckpointLedger(65) == 128);
    CHECK(HistoryMaster::nextCheckpointLedger(66) == 128);
    CHECK(HistoryMaster::nextCheckpointLedger(126) == 128);
    CHECK(HistoryMaster::nextCheckpointLedger(127) == 128);
    CHECK(HistoryMaster::nextCheckpointLedger(128) == 128);
    CHECK(HistoryMaster::nextCheckpointLedger(129) == 192);
    CHECK(HistoryMaster::nextCheckpointLedger(130) == 192);
}

TEST_CASE_METHOD(HistoryTests, "HistoryMaster::compress", "[history]")
{
    std::string s = "hello there";
    HistoryMaster &hm = app.getHistoryMaster();
    std::string fname = hm.localFilename("compressme");
    {
        std::ofstream out(fname, std::ofstream::binary);
        out.write(s.data(), s.size());
    }
    bool done = false;
    hm.compress(
        fname,
        [&done, &fname, &hm](asio::error_code const& ec)
        {
            std::string compressed = fname + ".gz";
            CHECK(!fs::exists(fname));
            CHECK(fs::exists(compressed));
            hm.decompress(
                compressed,
                [&done, &fname, compressed](asio::error_code const& ec)
                {
                    CHECK(fs::exists(fname));
                    CHECK(!fs::exists(compressed));
                    done = true;
                });
        });
    crankTillDone(done);
}


TEST_CASE_METHOD(HistoryTests, "HistoryMaster::verifyHash", "[history]")
{
    std::string s = "hello there";
    HistoryMaster &hm = app.getHistoryMaster();
    std::string fname = hm.localFilename("hashme");
    {
        std::ofstream out(fname, std::ofstream::binary);
        out.write(s.data(), s.size());
    }
    bool done = false;
    uint256 hash = hexToBin256("12998c017066eb0d2a70b94e6ed3192985855ce390f321bbdb832022888bd251");
    hm.verifyHash(
        fname, hash,
        [&done](asio::error_code const& ec)
        {
            CHECK(!ec);
            done = true;
        });
    crankTillDone(done);
}


TEST_CASE_METHOD(HistoryTests, "HistoryArchiveState::get_put", "[history]")
{
    HistoryArchiveState has;
    has.currentLedger = 0x1234;
    bool done = false;

    auto i = app.getConfig().HISTORY.find("test");
    CHECK(i != app.getConfig().HISTORY.end());
    auto archive = i->second;

    auto& theApp = this->app; // need a local scope reference
    archive->putState(
        app, has,
        [&done, &theApp, archive](asio::error_code const& ec)
        {
            CHECK(!ec);
            archive->getState(
                theApp,
                [&done](asio::error_code const& ec,
                        HistoryArchiveState const& has2)
                {
                    CHECK(!ec);
                    CHECK(has2.currentLedger == 0x1234);
                    done = true;
                });
        });
    crankTillDone(done);
}


extern LedgerEntry
generateValidLedgerEntry();

void
HistoryTests::generateAndPublishHistory(size_t nPublishes)
{

    app.start();

    auto& lm = app.getLedgerMaster();
    auto& hm = app.getHistoryMaster();

    // At this point LCL should be 1, current ledger should be 2
    assert(lm.getLastClosedLedgerHeader().header.ledgerSeq == 1);
    assert(lm.getCurrentLedgerHeader().ledgerSeq == 2);

    SecretKey root = txtest::getRoot();
    SecretKey alice = txtest::getAccount("alice");
    SecretKey bob = txtest::getAccount("bob");
    SecretKey carol = txtest::getAccount("carol");

    uint32_t ledgerSeq = 1;
    uint64_t closeTime = 1;
    uint64_t minBalance = lm.getMinBalance(5);
    SequenceNumber rseq = txtest::getAccountSeqNum(root, app) + 1;

    while (hm.getPublishSuccessCount() < nPublishes)
    {
        uint64_t startCount = hm.getPublishStartCount();
        while (hm.getPublishStartCount() == startCount)
        {
            TxSetFramePtr txSet = std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);

            uint64_t big = minBalance + ledgerSeq;
            uint64_t small = 100 + ledgerSeq;

            // Root sends to alice every tx, bob every other tx, carol every 4rd tx.
            txSet->add(txtest::createPaymentTx(root, alice, rseq++, big));
            txSet->add(txtest::createPaymentTx(root, bob, rseq++, big));
            txSet->add(txtest::createPaymentTx(root, carol, rseq++, big));

            // They all randomly send a little to one another every ledger after #4
            if (ledgerSeq > 4)
            {
                SequenceNumber aseq = txtest::getAccountSeqNum(alice, app) + 1;
                SequenceNumber bseq = txtest::getAccountSeqNum(bob, app) + 1;
                SequenceNumber cseq = txtest::getAccountSeqNum(carol, app) + 1;

                if (flip()) txSet->add(txtest::createPaymentTx(alice, bob, aseq++, small));
                if (flip()) txSet->add(txtest::createPaymentTx(alice, carol, aseq++, small));

                if (flip()) txSet->add(txtest::createPaymentTx(bob, alice, bseq++, small));
                if (flip()) txSet->add(txtest::createPaymentTx(bob, carol, bseq++, small));

                if (flip()) txSet->add(txtest::createPaymentTx(carol, alice, cseq++, small));
                if (flip()) txSet->add(txtest::createPaymentTx(carol, bob, cseq++, small));
            }
            CLOG(DEBUG, "History") << "Closing synthetic ledger with " << txSet->size() << " txs";
            lm.closeLedger(LedgerCloseData(ledgerSeq++, txSet, closeTime++, 10));

            mLedgerSeqs.push_back(lm.getLastClosedLedgerHeader().header.ledgerSeq);
            mLedgerHashes.push_back(lm.getLastClosedLedgerHeader().hash);
            mBucket0Hashes.push_back(app.getCLFMaster().getBucketList().getLevel(0).getCurr()->getHash());
            mBucket1Hashes.push_back(app.getCLFMaster().getBucketList().getLevel(2).getCurr()->getHash());

        }

        CHECK(lm.getCurrentLedgerHeader().ledgerSeq == ledgerSeq + 1);

        // Advance until we've published (or failed to!)
        while (hm.getPublishSuccessCount() < hm.getPublishStartCount())
        {
            CHECK(hm.getPublishFailureCount() == 0);
            app.getClock().crank(false);
        }
    }

    // At this point LCL (modulo checkpoint frequency) should be 63 and we
    // should be starting in on ledger 0 (a.k.a. 64)...
    CHECK(lm.getCurrentLedgerHeader().ledgerSeq == (nPublishes * HistoryMaster::kCheckpointFrequency));

    CHECK(hm.getPublishFailureCount() == 0);
    CHECK(hm.getPublishSuccessCount() == nPublishes);
}

Application::pointer
HistoryTests::catchupNewApplication(uint32_t lastLedger,
                                    uint32_t initLedger,
                                    Config::TestDbMode dbMode,
                                    HistoryMaster::ResumeMode resumeMode,
                                    std::string const& appName,
                                    Application::pointer app2)
{

    CLOG(INFO, "History") << "****";
    CLOG(INFO, "History") << "**** Beginning catchup test for app '" << appName << "'";
    CLOG(INFO, "History") << "****";

    if (!app2)
    {
        mCfgs.emplace_back(getTestConfig(static_cast<int>(mCfgs.size()) + 1, dbMode));
        app2 = Application::create(clock, addLocalDirHistoryArchive(dir, mCfgs.back(), false));
        app2->start();
    }

    bool done = false;
    uint32_t nextLedger = 0;
    app2->getHistoryMaster().catchupHistory(
        lastLedger,
        initLedger,
        resumeMode,
        [&done, &nextLedger](asio::error_code const& ec, uint32_t next)
        {
            CLOG(INFO, "History") << "Caught up: nextLedger = " << next;
            nextLedger = next;
            CHECK(!ec);
            done = true;
        });

    assert(!app2->getClock().getIOService().stopped());

    while (!done &&
           !app2->getClock().getIOService().stopped())
    {
        app2->getClock().crank(false);
    }

    CLOG(INFO, "History") << "Caught up: initLedger = " << initLedger;
    CLOG(INFO, "History") << "Caught up: " << mLedgerSeqs.size()
                          << " ledgers, covering "
                          << "[" << mLedgerSeqs.front() << ", " << mLedgerSeqs.back() << "]" ;

    assert(nextLedger != 0);
    size_t i = nextLedger - 3;

    auto wantSeq = mLedgerSeqs.at(i);
    auto wantHash = mLedgerHashes.at(i);
    auto wantBucket0Hash = mBucket0Hashes.at(i);
    auto wantBucket1Hash = mBucket1Hashes.at(i);

    auto haveSeq = app2->getLedgerMaster().getLastClosedLedgerHeader().header.ledgerSeq;
    auto haveHash = app2->getLedgerMaster().getLastClosedLedgerHeader().hash;
    auto haveBucket0Hash = app2->getCLFMaster().getBucketList().getLevel(0).getCurr()->getHash();
    auto haveBucket1Hash = app2->getCLFMaster().getBucketList().getLevel(2).getCurr()->getHash();

    CLOG(INFO, "History") << "Caught up: want Seq[" << i << "] = " << wantSeq;
    CLOG(INFO, "History") << "Caught up: have Seq[" << i << "] = " << haveSeq;

    CLOG(INFO, "History") << "Caught up: want Hash[" << i << "] = " << hexAbbrev(wantHash);
    CLOG(INFO, "History") << "Caught up: have Hash[" << i << "] = " << hexAbbrev(haveHash);

    CLOG(INFO, "History") << "Caught up: want Bucket0Hash[" << i << "] = " << hexAbbrev(wantBucket0Hash);
    CLOG(INFO, "History") << "Caught up: have Bucket0Hash[" << i << "] = " << hexAbbrev(haveBucket0Hash);

    CLOG(INFO, "History") << "Caught up: want Bucket1Hash[" << i << "] = " << hexAbbrev(wantBucket1Hash);
    CLOG(INFO, "History") << "Caught up: have Bucket1Hash[" << i << "] = " << hexAbbrev(haveBucket1Hash);

    CHECK(nextLedger == haveSeq + 1);
    CHECK(wantSeq == haveSeq);
    CHECK(wantHash == haveHash);

    CHECK(app2->getCLFMaster().getBucketByHash(wantBucket0Hash));
    CHECK(app2->getCLFMaster().getBucketByHash(wantBucket1Hash));
    CHECK(wantBucket0Hash == haveBucket0Hash);
    CHECK(wantBucket1Hash == haveBucket1Hash);

    return app2;
}


TEST_CASE_METHOD(HistoryTests, "History publish", "[history]")
{
    generateAndPublishHistory(1);
}

TEST_CASE_METHOD(HistoryTests, "History catchup", "[history][historycatchup]")
{
    generateAndPublishHistory(3);

    uint32_t lastLedger = 0;
    uint32_t initLedger = app.getLedgerMaster().getCurrentLedgerHeader().ledgerSeq;

    std::vector<Application::pointer> apps;

    SECTION("full, RESUME_AT_LAST, IN_MEMORY_SQLITE")
    {
        apps.push_back(
            catchupNewApplication(
                lastLedger, initLedger,
                Config::TESTDB_IN_MEMORY_SQLITE,
                HistoryMaster::RESUME_AT_LAST,
                "full, RESUME_AT_LAST, IN_MEMORY_SQLITE"));
    }

    SECTION("full, RESUME_AT_LAST, ON_DISK_SQLITE")
    {
        apps.push_back(
            catchupNewApplication(
                lastLedger, initLedger,
                Config::TESTDB_ON_DISK_SQLITE,
                HistoryMaster::RESUME_AT_LAST,
                "full, RESUME_AT_LAST, ON_DISK_SQLITE"));
    }

#ifdef USE_POSTGRES
    SECTION("full, RESUME_AT_LAST, TCP_LOCALHOST_POSTGRESQL")
    {
        apps.push_back(
            catchupNewApplication(
                lastLedger, initLedger,
                Config::TESTDB_TCP_LOCALHOST_POSTGRESQL,
                HistoryMaster::RESUME_AT_LAST,
                "full, RESUME_AT_LAST, TCP_LOCALHOST_POSTGRESQL"));
    }
#endif

}
