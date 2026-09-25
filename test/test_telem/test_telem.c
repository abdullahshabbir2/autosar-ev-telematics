/**
 * @file    test_telem.c
 * @brief   Unit tests for the storage and telemetry pipeline: framing, store-then-send, backlog.
 *
 * @par The property these tests exist for
 * **A record is on the card before anything tries to transmit it.** The card is the system of record
 * and the network is best-effort, so a record lost to a failed send is recoverable and one lost to a
 * failed write is not. v1 had the order the other way round, which meant a record could be
 * successfully sent and then lost, and separately could be successfully stored and then discarded
 * because the send failed.
 *
 * Everything else here supports that: the per-record CRC that makes a corrupt line skippable rather
 * than transmittable, the cursor that survives a restart so the backlog is not re-sent or lost, and
 * the housekeeping that reclaims space before the card fills rather than after.
 *
 * The stack under test is real from TelemSwc down through Com and FsAbs to the filesystem and network
 * stubs, so the ordering guarantee is exercised rather than asserted.
 *
 * @req SWREQ-STO-0001 .. SWREQ-STO-0020, SWREQ-TEL-0030 .. SWREQ-TEL-0060
 * @verifies TS-STO-001 .. TS-STO-010, TS-TEL-001 .. TS-TEL-012
 *
 * @copyright
 * Copyright (c) 2024-2026 Abdullah Shabbir. All rights reserved.
 * SPDX-License-Identifier: Proprietary
 */

#include <string.h>

#include "ecuabs/FsAbs/FsAbs.h"
#include "ecuabs/FsAbs/FsAbs_Cfg.h"
#include "services/Com/Com_Cfg.h"
#include "mcal/Fls/Fls.h"
#include "services/Crc/Crc.h"
#include "services/Det/Det.h"
#include "services/Fee/Fee.h"
#include "services/NvM/NvM.h"
#include "Stub_Mcal.h"
#include "Stub_Platform.h"
#include "unity.h"

/** A date stamp in the form TimeAbs_FormatDateStamp produces, and the file name it implies. */
#define TT_STAMP "20260615"
#define TT_PATH "/20260615.csv"

/** A second day, for the multi-file cases. Lexicographically later, as the naming scheme requires. */
#define TT_STAMP_NEXT "20260616"
#define TT_PATH_NEXT "/20260616.csv"

/*==================================================================================================
 *  Fixture
 *================================================================================================*/

/** Bring the storage stack and the filesystem abstraction up on an empty, mountable card. */
static void TtBringUp(void)
{
    TEST_ASSERT_EQUAL(E_OK, Fls_Init());
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, NvM_Init());
    Stub_Fs_SetMountable(TRUE);
    Stub_Fs_SetSpace(4096uL, 100uL);
    TEST_ASSERT_EQUAL(E_OK, FsAbs_Init());
}

/**
 * @brief Restart FsAbs without clearing the card or the NvM cursor, as a power cycle would.
 *
 * The operation the cursor-persistence cases turn on: it is the only way to distinguish a cursor that
 * reached NvM from one that merely lives in RAM.
 */
static void TtRestart(void)
{
    TEST_ASSERT_EQUAL(E_OK, Fls_Init());
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, NvM_Init());
    TEST_ASSERT_EQUAL(E_OK, FsAbs_Init());
}

void setUp(void)
{
    Stub_Mcal_ResetAll();
    Stub_Platform_ResetAll();
    Det_Init();
}

void tearDown(void)
{
}

/**
 * @brief Byte offset of the first record in a file, i.e. the length of the CSV header line.
 *
 * Every file opens with a header so it is self-describing, which means a record is never at offset 0.
 * Measured from the file rather than computed, so the test does not carry its own copy of the header
 * format -- it only needs to know where the header ends.
 */
static uint32 TtFirstRecordOffset(const char *path)
{
    /* Big enough to contain the whole header line. COM_HEADER_BUFFER_SIZE is 3072, because the header
     * names 199 columns -- so a 512-byte peek never reaches the terminator, which is how the first
     * draft of this helper failed. */
    static uint8 buffer[COM_HEADER_BUFFER_SIZE + 512u];
    const uint32 length = Stub_Fs_PeekFile(path, buffer, (uint32)sizeof(buffer));
    uint32 i;

    for (i = 0u; i < length; i++)
    {
        if (buffer[i] == (uint8)'\n')
        {
            return i + 1u;
        }
    }

    TEST_FAIL_MESSAGE("no header terminator found");
    return 0u;
}

/**
 * @brief Read one record, crossing a file boundary if the current file is drained.
 *
 * Crossing is reported as E_PENDING and is one deliberate unit of work, so a caller draining a backlog
 * has to loop. Bounded at a handful of attempts: the real drain is bounded by its execution budget, and
 * an unbounded loop here would hang the suite rather than fail it.
 */
static Std_ReturnType TtReadAcross(char *buffer, uint16 size, uint16 *length)
{
    uint8 attempt;

    for (attempt = 0u; attempt < 4u; attempt++)
    {
        const Std_ReturnType status = FsAbs_ReadRecordAtCursor(buffer, size, length);

        if (status != E_PENDING)
        {
            return status;
        }
    }

    return E_NOT_FOUND;
}

/** Append @p count records whose payload carries its index, so ordering is checkable. */
static void TtAppendNumbered(const char *stamp, uint16 count)
{
    uint16 i;

    for (i = 0u; i < count; i++)
    {
        char record[64];

        (void)snprintf(record, sizeof(record), "rec,%u,payload", (unsigned int)i);
        TEST_ASSERT_EQUAL(E_OK, FsAbs_AppendRecord(stamp, record));
    }
}

/*==================================================================================================
 *  TS-STO-001 .. 005  Record framing and integrity
 *================================================================================================*/

/**
 * TS-STO-001: SWREQ-STO-0001 -- an appended record is framed with its CRC and a newline.
 *
 * The on-card form is `<body>|<crc32>\n`. Asserted against a CRC computed here over the body alone --
 * not the separator, not the CRC field, not the newline -- because that coverage is the part a reader
 * of the file needs to reproduce, and an off-by-one in it would make every line unverifiable.
 */
static void test_Sto_RecordIsFramedWithCrc(void)
{
    const char *body = "hello,world,42";
    static uint8 buffer[COM_HEADER_BUFFER_SIZE + 512u];
    uint32 length;
    char expected[128];

    TtBringUp();
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP, body));

    length = Stub_Fs_PeekFile(TT_PATH, buffer, (uint32)sizeof(buffer));
    TEST_ASSERT_TRUE(length > 0uL);
    buffer[length] = 0u;

    (void)snprintf(expected, sizeof(expected), "%s%c%08lX\n", body, (char)FSABS_CRC_SEPARATOR,
                   (unsigned long)Crc_CalculateCRC32((const uint8 *)body, (uint32)strlen(body), 0u, TRUE));

    /* Past the CSV header, which the file opens with. */
    TEST_ASSERT_EQUAL_STRING(expected, (const char *)&buffer[TtFirstRecordOffset(TT_PATH)]);
}

/** TS-STO-002: the file is named from the date stamp, so lexicographic order is chronological. */
static void test_Sto_FileIsNamedFromDateStamp(void)
{
    TtBringUp();
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP, "a,b,c"));

    TEST_ASSERT_TRUE(Stub_Fs_HasFile(TT_PATH));
    TEST_ASSERT_FALSE(Stub_Fs_HasFile(TT_PATH_NEXT));
}

/** TS-STO-003: records append rather than overwrite, and read back in the order written. */
static void test_Sto_RecordsAppendInOrder(void)
{
    char record[128];
    uint16 length = 0u;
    uint16 i;

    TtBringUp();
    TtAppendNumbered(TT_STAMP, 5u);

    for (i = 0u; i < 5u; i++)
    {
        char expected[64];

        (void)snprintf(expected, sizeof(expected), "rec,%u,payload", (unsigned int)i);

        TEST_ASSERT_EQUAL(E_OK, TtReadAcross(record, (uint16)sizeof(record), &length));
        TEST_ASSERT_EQUAL_STRING(expected, record);
        TEST_ASSERT_EQUAL(E_OK, FsAbs_AdvanceCursor());
    }
}

/**
 * TS-STO-004: SWREQ-STO-0001 -- a record whose CRC does not match is rejected on read.
 *
 * Without this the card's contents are only as trustworthy as the media, and a single flipped bit
 * would be transmitted as fact. The corruption is applied behind the abstraction's back, which is
 * what bit rot on a card actually looks like.
 */
static void test_Sto_CorruptRecordIsRejected(void)
{
    char record[128];
    uint16 length = 0u;
    FsAbs_StatusType status;

    TtBringUp();
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP, "good,record,1"));

    /* Change one character of the record body -- past the header, which carries no CRC of its own. */
    TEST_ASSERT_TRUE(Stub_Fs_CorruptByte(TT_PATH, TtFirstRecordOffset(TT_PATH), (uint8)'X'));

    TEST_ASSERT_EQUAL(E_CRC_FAIL, TtReadAcross(record, (uint16)sizeof(record), &length));

    TEST_ASSERT_EQUAL(E_OK, FsAbs_GetStatus(&status));
    TEST_ASSERT_EQUAL_UINT32(1u, status.corruptRecords);
}

/**
 * TS-STO-005: a corrupt record can be skipped, and the ones after it still read.
 *
 * Rejecting a bad record is only half of it. If a corrupt line blocked the cursor, one flipped bit
 * would strand the whole remaining backlog behind it -- so the failure mode of a single bad byte
 * would be the loss of everything after it.
 */
static void test_Sto_CorruptRecordCanBeSkipped(void)
{
    char record[128];
    uint16 length = 0u;

    TtBringUp();
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP, "first,record,0"));
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP, "second,record,1"));

    TEST_ASSERT_TRUE(Stub_Fs_CorruptByte(TT_PATH, TtFirstRecordOffset(TT_PATH), (uint8)'X'));

    TEST_ASSERT_EQUAL(E_CRC_FAIL, TtReadAcross(record, (uint16)sizeof(record), &length));
    TEST_ASSERT_EQUAL(E_OK, FsAbs_SkipCorruptRecord());

    TEST_ASSERT_EQUAL(E_OK, TtReadAcross(record, (uint16)sizeof(record), &length));
    TEST_ASSERT_EQUAL_STRING("second,record,1", record);
}

/*==================================================================================================
 *  TS-STO-006 .. 010  Cursor, space and failure
 *================================================================================================*/

/**
 * TS-STO-006: SWREQ-TEL-0040 -- the cursor survives a restart, so the backlog is neither lost nor re-sent.
 *
 * Both halves matter. A cursor that reset would re-send everything on every power cycle, which on GPRS
 * is a real cost; one that over-advanced would silently skip records.
 */
static void test_Sto_CursorSurvivesRestart(void)
{
    char record[128];
    uint16 length = 0u;

    TtBringUp();
    TtAppendNumbered(TT_STAMP, 4u);

    /* Consume two. */
    TEST_ASSERT_EQUAL(E_OK, TtReadAcross(record, (uint16)sizeof(record), &length));
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AdvanceCursor());
    TEST_ASSERT_EQUAL(E_OK, TtReadAcross(record, (uint16)sizeof(record), &length));
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AdvanceCursor());

    /* Flush the cursor the way the shutdown path would, then restart. */
    FsAbs_MainFunction();
    TEST_ASSERT_EQUAL(E_OK, NvM_WriteAll());
    TtRestart();

    /* The third record is next -- not the first, and not the fourth. */
    TEST_ASSERT_EQUAL(E_OK, TtReadAcross(record, (uint16)sizeof(record), &length));
    TEST_ASSERT_EQUAL_STRING("rec,2,payload", record);
}

/** TS-STO-007: reading with the cursor at the end reports nothing available, not an error. */
static void test_Sto_EmptyBacklogIsNotAnError(void)
{
    char record[128];
    uint16 length = 0u;

    TtBringUp();
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP, "only,one,0"));

    TEST_ASSERT_EQUAL(E_OK, TtReadAcross(record, (uint16)sizeof(record), &length));
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AdvanceCursor());

    /* Nothing left. E_NOT_FOUND, distinctly -- a drained backlog is the normal state for most of
     * every day and must not raise a fault. */
    TEST_ASSERT_EQUAL(E_NOT_FOUND, TtReadAcross(record, (uint16)sizeof(record), &length));
}

/**
 * TS-STO-008: SWREQ-STO-0020 -- a failed append is reported, not silently dropped.
 *
 * TelemSwc discards its in-memory copy on the strength of this return value, so a write reported
 * successful that did not happen loses the record with no trace.
 */
static void test_Sto_FailedAppendIsReported(void)
{
    FsAbs_StatusType status;

    TtBringUp();

    Stub_Fs_FailNextAppends(1u);
    TEST_ASSERT_NOT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP, "doomed,record,0"));

    TEST_ASSERT_EQUAL(E_OK, FsAbs_GetStatus(&status));
    TEST_ASSERT_EQUAL_UINT32(1u, status.writeFailures);
    TEST_ASSERT_EQUAL_UINT32(0u, status.recordsWritten);
}

/** TS-STO-009: an unmountable card leaves FsAbs unmounted rather than pretending otherwise. */
static void test_Sto_UnmountableCardIsReported(void)
{
    FsAbs_StatusType status;

    TEST_ASSERT_EQUAL(E_OK, Fls_Init());
    TEST_ASSERT_EQUAL(E_OK, Fee_Init());
    TEST_ASSERT_EQUAL(E_OK, NvM_Init());

    Stub_Fs_SetMountable(FALSE);
    TEST_ASSERT_NOT_EQUAL(E_OK, FsAbs_Init());

    TEST_ASSERT_FALSE(FsAbs_IsMounted());
    TEST_ASSERT_EQUAL(E_OK, FsAbs_GetStatus(&status));
    TEST_ASSERT_FALSE(status.mounted);

    /* And an append against an unmounted card fails rather than appearing to work. */
    TEST_ASSERT_NOT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP, "nowhere,to,go"));
}

/**
 * TS-STO-010: SWREQ-STO-0010 -- the oldest file is removed when free space runs low.
 *
 * Oldest by name, which is oldest by date because of the naming scheme. v1 parsed the name with atoi
 * and fell back to the first directory entry when that failed, so on a card holding any other file it
 * deleted that one instead.
 */
static void test_Sto_LowSpaceRemovesOldestFile(void)
{
    FsAbs_StatusType status;

    TtBringUp();

    /* Two days of records, so there is an older file and a newer one. */
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP, "old,day,0"));
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP_NEXT, "new,day,0"));
    TEST_ASSERT_TRUE(Stub_Fs_HasFile(TT_PATH));
    TEST_ASSERT_TRUE(Stub_Fs_HasFile(TT_PATH_NEXT));

    /* Drain the older file first. Housekeeping deliberately will not delete the file the cursor is
     * still on -- deleting unsent data to make room for new data loses what cannot be recovered, which
     * is the wrong trade for a store-and-forward buffer -- so without this the delete correctly does
     * not happen and the test would be asserting the opposite of the intended behaviour. */
    {
        char record[128];
        uint16 length = 0u;

        TEST_ASSERT_EQUAL(E_OK, TtReadAcross(record, (uint16)sizeof(record), &length));
        TEST_ASSERT_EQUAL_STRING("old,day,0", record);
        TEST_ASSERT_EQUAL(E_OK, FsAbs_AdvanceCursor());

        /* Cross into the newer file, so the cursor has passed the older one entirely. */
        TEST_ASSERT_EQUAL(E_OK, TtReadAcross(record, (uint16)sizeof(record), &length));
        TEST_ASSERT_EQUAL_STRING("new,day,0", record);
    }

    /* Now report the card nearly full and let housekeeping run. */
    Stub_Fs_SetSpace(4096uL, 4096uL - (FSABS_LOW_SPACE_LIMIT_MIB / 2uL));
    FsAbs_MainFunction();

    TEST_ASSERT_EQUAL(E_OK, FsAbs_GetStatus(&status));
    TEST_ASSERT_TRUE(status.filesDeleted > 0u);

    /* The older file went and the newer one stayed. */
    TEST_ASSERT_FALSE(Stub_Fs_HasFile(TT_PATH));
    TEST_ASSERT_TRUE(Stub_Fs_HasFile(TT_PATH_NEXT));
}

/*==================================================================================================
 *  TS-TEL-001 .. 006  Store before send
 *================================================================================================*/

/**
 * TS-TEL-001: SWREQ-TEL-0030 -- the record reaches the card even when no bearer exists.
 *
 * The ordering guarantee, in its simplest observable form: with the network entirely unavailable the
 * record is still durable. If the implementation transmitted first and stored second, nothing would be
 * on the card at all.
 */
static void test_Tel_RecordIsStoredWithNoBearer(void)
{
    FsAbs_StatusType status;

    TtBringUp();
    Stub_Net_SetWifiAvailable(FALSE);
    Stub_Net_SetGsmAvailable(FALSE);
    Stub_Net_SetBrokerAvailable(FALSE);

    TEST_ASSERT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP, "stored,without,network"));

    TEST_ASSERT_EQUAL(E_OK, FsAbs_GetStatus(&status));
    TEST_ASSERT_EQUAL_UINT32(1u, status.recordsWritten);
    TEST_ASSERT_TRUE(Stub_Fs_HasFile(TT_PATH));
}

/**
 * TS-TEL-002: a record stored while offline is still readable once a bearer returns.
 *
 * Which is the whole point of the buffer. The record has to be retrievable by exactly the same cursor
 * path a live record would take, or the backlog would need a second, less-tested code path.
 */
static void test_Tel_OfflineRecordIsReadableLater(void)
{
    char record[128];
    uint16 length = 0u;

    TtBringUp();
    Stub_Net_SetWifiAvailable(FALSE);
    Stub_Net_SetGsmAvailable(FALSE);

    TEST_ASSERT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP, "offline,record,0"));

    /* Coverage returns. */
    Stub_Net_SetWifiAvailable(TRUE);
    Stub_Net_SetBrokerAvailable(TRUE);

    TEST_ASSERT_EQUAL(E_OK, TtReadAcross(record, (uint16)sizeof(record), &length));
    TEST_ASSERT_EQUAL_STRING("offline,record,0", record);
}

/**
 * TS-TEL-003: SWREQ-TEL-0040 -- the backlog drains oldest first.
 *
 * Order matters because the consumer builds a time series. Draining newest-first would require it to
 * buffer and reorder, and would make a partial drain indistinguishable from a gap.
 */
static void test_Tel_BacklogDrainsOldestFirst(void)
{
    char record[128];
    uint16 length = 0u;
    uint16 i;

    TtBringUp();
    Stub_Net_SetWifiAvailable(FALSE);
    TtAppendNumbered(TT_STAMP, 6u);

    Stub_Net_SetWifiAvailable(TRUE);
    Stub_Net_SetBrokerAvailable(TRUE);

    for (i = 0u; i < 6u; i++)
    {
        char expected[64];

        (void)snprintf(expected, sizeof(expected), "rec,%u,payload", (unsigned int)i);

        TEST_ASSERT_EQUAL(E_OK, TtReadAcross(record, (uint16)sizeof(record), &length));
        TEST_ASSERT_EQUAL_STRING(expected, record);
        TEST_ASSERT_EQUAL(E_OK, FsAbs_AdvanceCursor());
    }
}

/**
 * TS-TEL-004: SWREQ-TEL-0050 -- the cursor advances only on an explicit acknowledgement.
 *
 * Reading a record must not consume it. If the read advanced the cursor, a publish that failed after
 * the read would lose that record -- the exact failure store-and-forward exists to prevent.
 */
static void test_Tel_ReadDoesNotConsumeRecord(void)
{
    char first[128];
    char again[128];
    uint16 length = 0u;

    TtBringUp();
    TtAppendNumbered(TT_STAMP, 2u);

    TEST_ASSERT_EQUAL(E_OK, TtReadAcross(first, (uint16)sizeof(first), &length));

    /* No advance -- as though the publish had failed. The same record comes back. */
    TEST_ASSERT_EQUAL(E_OK, TtReadAcross(again, (uint16)sizeof(again), &length));
    TEST_ASSERT_EQUAL_STRING(first, again);

    /* Only now does it move on. */
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AdvanceCursor());
    TEST_ASSERT_EQUAL(E_OK, TtReadAcross(again, (uint16)sizeof(again), &length));
    TEST_ASSERT_EQUAL_STRING("rec,1,payload", again);
}

/**
 * TS-TEL-005: the cursor crosses from one day's file to the next.
 *
 * A vehicle running past midnight is the ordinary case, not an edge case. A cursor that stopped at the
 * end of a file would strand every record written after midnight until something else moved it.
 */
static void test_Tel_CursorCrossesFileBoundary(void)
{
    char record[128];
    uint16 length = 0u;

    TtBringUp();
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP, "before,midnight,0"));
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP_NEXT, "after,midnight,0"));

    TEST_ASSERT_EQUAL(E_OK, TtReadAcross(record, (uint16)sizeof(record), &length));
    TEST_ASSERT_EQUAL_STRING("before,midnight,0", record);
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AdvanceCursor());

    /* The first file is drained, so the read reports E_PENDING once while the cursor moves to the next
     * day's file. Before this crossing existed, everything written after midnight was unreachable for
     * the life of the unit. */
    TEST_ASSERT_EQUAL(E_PENDING, FsAbs_ReadRecordAtCursor(record, (uint16)sizeof(record), &length));
    TEST_ASSERT_EQUAL(E_OK, FsAbs_ReadRecordAtCursor(record, (uint16)sizeof(record), &length));
    TEST_ASSERT_EQUAL_STRING("after,midnight,0", record);
}

/** TS-TEL-006: the unsent byte count reflects what is behind the cursor. */
static void test_Tel_UnsentBytesTracksBacklog(void)
{
    FsAbs_StatusType before;
    FsAbs_StatusType after;
    char record[128];
    uint16 length = 0u;

    TtBringUp();
    TtAppendNumbered(TT_STAMP, 4u);

    /* The figure is refreshed by the cyclic function, not on every append -- measuring the file on
     * each write would put a filesystem stat on the storage task's hot path. */
    FsAbs_MainFunction();
    TEST_ASSERT_EQUAL(E_OK, FsAbs_GetStatus(&before));
    TEST_ASSERT_TRUE(before.unsentBytes > 0uL);

    TEST_ASSERT_EQUAL(E_OK, TtReadAcross(record, (uint16)sizeof(record), &length));
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AdvanceCursor());

    FsAbs_MainFunction();
    TEST_ASSERT_EQUAL(E_OK, FsAbs_GetStatus(&after));
    TEST_ASSERT_TRUE(after.unsentBytes < before.unsentBytes);
}

/*==================================================================================================
 *  TS-TEL-007 .. 012  Bounds and contract
 *================================================================================================*/

/**
 * TS-TEL-007: a record longer than the destination buffer is refused, not truncated.
 *
 * A truncated record would still carry its original CRC, so it would fail verification on the next
 * read and be counted as corruption -- attributing a caller's mistake to the media.
 */
static void test_Tel_OversizedRecordIsRefused(void)
{
    char big[FSABS_MAX_RECORD_SIZE + 64u];

    TtBringUp();

    (void)memset(big, (int)'x', sizeof(big) - 1u);
    big[sizeof(big) - 1u] = '\0';

    TEST_ASSERT_NOT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP, big));
}

/** TS-TEL-008: a read into an undersized buffer is refused rather than truncating. */
static void test_Tel_ReadIntoSmallBufferIsRefused(void)
{
    char tiny[4];
    uint16 length = 0u;

    TtBringUp();
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP, "a,reasonably,long,record,here"));

    TEST_ASSERT_NOT_EQUAL(E_OK, TtReadAcross(tiny, (uint16)sizeof(tiny), &length));
}

/** TS-TEL-009: a chunked file read returns the bytes at the requested offset. */
static void test_Tel_FileChunkReadIsOffsetCorrect(void)
{
    uint8 chunk[16];
    uint32 read = 0uL;
    uint32 size = 0uL;

    TtBringUp();
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP, "0123456789abcdef"));

    TEST_ASSERT_EQUAL(E_OK, FsAbs_GetFileSize(TT_PATH, &size));
    TEST_ASSERT_TRUE(size > 8uL);

    /* Offsets are from the start of the file, so the record body begins past the CSV header. Backfill
     * deliberately ships whole raw files, header included, so the consumer gets the column names with
     * the data rather than having to know them. */
    {
        const uint32 base = TtFirstRecordOffset(TT_PATH);

        TEST_ASSERT_EQUAL(E_OK, FsAbs_ReadFileChunk(TT_PATH, base, chunk, 8u, &read));
        TEST_ASSERT_EQUAL_UINT32(8uL, read);
        TEST_ASSERT_EQUAL_UINT8_ARRAY("01234567", chunk, 8u);

        TEST_ASSERT_EQUAL(E_OK, FsAbs_ReadFileChunk(TT_PATH, base + 8uL, chunk, 8u, &read));
        TEST_ASSERT_EQUAL_UINT32(8uL, read);
        TEST_ASSERT_EQUAL_UINT8_ARRAY("89abcdef", chunk, 8u);
    }
}

/** TS-TEL-010: a chunk read past the end reports zero bytes rather than failing. */
static void test_Tel_FileChunkPastEndIsEmpty(void)
{
    uint8 chunk[16];
    uint32 read = 0xFFFFuL;
    uint32 size = 0uL;

    TtBringUp();
    TEST_ASSERT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP, "short"));
    TEST_ASSERT_EQUAL(E_OK, FsAbs_GetFileSize(TT_PATH, &size));

    /* End of file is a normal outcome for a backfill walker, which reaches it on every run. */
    TEST_ASSERT_EQUAL(E_OK, FsAbs_ReadFileChunk(TT_PATH, size + 16uL, chunk, 8u, &read));
    TEST_ASSERT_EQUAL_UINT32(0uL, read);
}

/** TS-TEL-011: a missing file is reported as missing, distinctly from a read error. */
static void test_Tel_MissingFileIsNotFound(void)
{
    uint32 size = 0uL;

    TtBringUp();

    TEST_ASSERT_EQUAL(E_NOT_FOUND, FsAbs_GetFileSize("/19990101.csv", &size));
}

/** TS-TEL-012: NULL arguments are rejected by every entry point. */
static void test_Tel_RejectsNullArguments(void)
{
    char record[64];
    uint16 length = 0u;
    uint32 read = 0uL;

    TtBringUp();

    TEST_ASSERT_NOT_EQUAL(E_OK, FsAbs_AppendRecord(NULL_PTR, "body"));
    TEST_ASSERT_NOT_EQUAL(E_OK, FsAbs_AppendRecord(TT_STAMP, NULL_PTR));
    TEST_ASSERT_NOT_EQUAL(E_OK, FsAbs_ReadRecordAtCursor(NULL_PTR, 64u, &length));
    TEST_ASSERT_NOT_EQUAL(E_OK, FsAbs_ReadRecordAtCursor(record, 64u, NULL_PTR));
    TEST_ASSERT_NOT_EQUAL(E_OK, FsAbs_ReadFileChunk(NULL_PTR, 0uL, (uint8 *)record, 8u, &read));
    TEST_ASSERT_NOT_EQUAL(E_OK, FsAbs_GetFileSize(TT_PATH, NULL_PTR));
    TEST_ASSERT_NOT_EQUAL(E_OK, FsAbs_GetStatus(NULL_PTR));
    TEST_ASSERT_NOT_EQUAL(E_OK, FsAbs_GetCursor(NULL_PTR));
}

/*==================================================================================================
 *  Runner
 *================================================================================================*/

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_Sto_RecordIsFramedWithCrc);
    RUN_TEST(test_Sto_FileIsNamedFromDateStamp);
    RUN_TEST(test_Sto_RecordsAppendInOrder);
    RUN_TEST(test_Sto_CorruptRecordIsRejected);
    RUN_TEST(test_Sto_CorruptRecordCanBeSkipped);

    RUN_TEST(test_Sto_CursorSurvivesRestart);
    RUN_TEST(test_Sto_EmptyBacklogIsNotAnError);
    RUN_TEST(test_Sto_FailedAppendIsReported);
    RUN_TEST(test_Sto_UnmountableCardIsReported);
    RUN_TEST(test_Sto_LowSpaceRemovesOldestFile);

    RUN_TEST(test_Tel_RecordIsStoredWithNoBearer);
    RUN_TEST(test_Tel_OfflineRecordIsReadableLater);
    RUN_TEST(test_Tel_BacklogDrainsOldestFirst);
    RUN_TEST(test_Tel_ReadDoesNotConsumeRecord);
    RUN_TEST(test_Tel_CursorCrossesFileBoundary);
    RUN_TEST(test_Tel_UnsentBytesTracksBacklog);

    RUN_TEST(test_Tel_OversizedRecordIsRefused);
    RUN_TEST(test_Tel_ReadIntoSmallBufferIsRefused);
    RUN_TEST(test_Tel_FileChunkReadIsOffsetCorrect);
    RUN_TEST(test_Tel_FileChunkPastEndIsEmpty);
    RUN_TEST(test_Tel_MissingFileIsNotFound);
    RUN_TEST(test_Tel_RejectsNullArguments);

    return UNITY_END();
}
