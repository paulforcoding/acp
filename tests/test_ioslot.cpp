#include "lib/thirdparty/catch2/catch_amalgamated.hpp"
#include "lib/combined/combined.hpp"
#include <cstring>

TEST_CASE("IOSlot construction and destruction")
{
    {
        IOSlot slot(4096, 0, "rw");
        REQUIRE(slot.GetID() == 0);
        REQUIRE(slot.GetType() == "rw");
        REQUIRE(slot.GetStatus() == IOSlot::Status::Init);
        REQUIRE(slot.GetBuf() != nullptr);
        REQUIRE(slot.GetCPFPPtr() == nullptr);
    }

    {
        IOSlot slot(8192, 5, "cksum");
        REQUIRE(slot.GetID() == 5);
        REQUIRE(slot.GetType() == "cksum");
    }
}

TEST_CASE("IOSlot StatusToStr covers all statuses")
{
    REQUIRE(std::strcmp(IOSlot::StatusToStr(IOSlot::Status::Init), "Init") == 0);
    REQUIRE(std::strcmp(IOSlot::StatusToStr(IOSlot::Status::ReadPrepared), "ReadPrepared") == 0);
    REQUIRE(std::strcmp(IOSlot::StatusToStr(IOSlot::Status::ReadSubmitted), "ReadSubmitted") == 0);
    REQUIRE(std::strcmp(IOSlot::StatusToStr(IOSlot::Status::ReadReaped), "ReadReaped") == 0);
    REQUIRE(std::strcmp(IOSlot::StatusToStr(IOSlot::Status::WritePrepared), "WritePrepared") == 0);
    REQUIRE(std::strcmp(IOSlot::StatusToStr(IOSlot::Status::WriteSubmitted), "WriteSubmitted") == 0);
    REQUIRE(std::strcmp(IOSlot::StatusToStr(IOSlot::Status::WriteReaped), "WriteReaped") == 0);
}

TEST_CASE("IOSlot IOInfo get and set")
{
    IOSlot slot(4096, 1, "rw");

    slot.SetIOInfo(1024, 2048);
    auto info = slot.GetIOInfo();

    REQUIRE(info.offset == 1024);
    REQUIRE(info.io_size == 2048);
}

TEST_CASE("IOSlot Reset clears state")
{
    IOSlot slot(4096, 2, "rw");

    slot.SetStatus(IOSlot::Status::ReadSubmitted);
    slot.SetIOInfo(100, 200);
    slot.SetUserData(42);

    slot.Reset();

    REQUIRE(slot.GetStatus() == IOSlot::Status::Init);
    auto info = slot.GetIOInfo();
    REQUIRE(info.offset == 0);
    REQUIRE(info.io_size == 0);
}

TEST_CASE("IOSlot AssociatedSlot")
{
    IOSlot slot1(4096, 0, "rw");
    IOSlot slot2(4096, 1, "cksum");

    REQUIRE(slot1.GetAssociatedSlot() == nullptr);

    slot1.SetAssociatedSlot(&slot2);
    REQUIRE(slot1.GetAssociatedSlot() == &slot2);

    slot1.SetAssociatedSlot(nullptr);
    REQUIRE(slot1.GetAssociatedSlot() == nullptr);
}

TEST_CASE("IOSlot iocb initialization")
{
    IOSlot slot(4096, 0, "rw");

    struct iocb* readIocb = slot.InitReadIOCB();
    REQUIRE(readIocb != nullptr);
    REQUIRE(readIocb == slot.GetReadIOCB());

    struct iocb* writeIocb = slot.InitWriteIOCB();
    REQUIRE(writeIocb != nullptr);
    REQUIRE(writeIocb == slot.GetWriteIOCB());
}

TEST_CASE("IOSlot CPFPPtr")
{
    IOSlot slot(4096, 0, "rw");
    REQUIRE(slot.GetCPFPPtr() == nullptr);

    auto mockFp = std::make_shared<CPFilePair>("", "", 4096, false, false, false, nullptr, nullptr);
    slot.SetCPFPPtr(mockFp);
    REQUIRE(slot.GetCPFPPtr() == mockFp);
}

TEST_CASE("IOSlot copy and move are deleted")
{
    REQUIRE(!std::is_copy_constructible_v<IOSlot>);
    REQUIRE(!std::is_copy_assignable_v<IOSlot>);
    REQUIRE(!std::is_move_constructible_v<IOSlot>);
    REQUIRE(!std::is_move_assignable_v<IOSlot>);
}
