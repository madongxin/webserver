#include "AoiEvidence.h"

#include <cstdio>
#include <sstream>
#include <string>

namespace {

int fails = 0;

void Expect(bool cond, const char *msg) {
    if (!cond) {
        std::printf("FAIL: %s\n", msg);
        ++fails;
    }
}

bool Has(const std::string &s, const char *needle) { return s.find(needle) != std::string::npos; }

int g_reads = 0;

bool FakeTcp(int, AoiTcpSample *out) {
    ++g_reads;
    *out = AoiTcpSample{};
    out->ok = true;
    out->retransmitting = true;
    out->retrans = 3;
    out->unacked = 4;
    out->has_backoff = true;
    out->backoff = 2;
    out->rto_us = 40000000;
    out->rtt_us = 1300000;
    out->snd_cwnd = 2;
    out->has_last_ack_recv = true;
    out->last_ack_recv_ms = 5000;
    out->has_last_data_sent = true;
    out->last_data_sent_ms = 80;
    out->has_bytes_acked = true;
    out->bytes_acked = 1000;
    out->has_bytes_retrans = false;
    out->has_total_retrans = true;
    out->total_retrans = 7;
    out->has_notsent_ioctl = true;
    out->notsent = 17088;
    return true;
}

bool PlayerOf(uint64_t conn, uint64_t *player) {
    if (conn != 7 || !player)
        return false;
    *player = 819;
    return true;
}

}  // namespace

int main() {
    AoiResetEvidenceForTest();
    AoiSetTcpSampleFnForTest(FakeTcp);
    AoiSetConnPlayerFn(PlayerOf);
    AoiSetEvidenceNowMsForTest(10000);
    AoiNoteTxBytes(AoiWriteFrame::Move, 700, 7);
    AoiNoteTxBytes(AoiWriteFrame::Reply, 200, 7);
    AoiNoteTxBytes(AoiWriteFrame::Enter, 50, 7);
    AoiNoteTxBytes(AoiWriteFrame::Leave, 30, 7);
    AoiNoteTxBytes(AoiWriteFrame::Heartbeat, 20, 7);
    AoiNoteWriteFail(7, AoiWriteFrame::Move, AoiWriteFailReason::QueueFull);
    AoiObserveBusySocket(7, 3, 100, 20000);
    Expect(g_reads == 1, "first sample reads TCP_INFO");
    AoiSetEvidenceNowMsForTest(10500);
    AoiObserveBusySocket(7, 3, 100, 20000);
    Expect(g_reads == 1, "same connection is not sampled twice inside one second");
    AoiSetEvidenceNowMsForTest(12000);
    AoiObserveBusySocket(7, 3, 110, 21000);
    Expect(g_reads == 2, "sample again after one second");

    const std::string json = AoiStalledJson(20);
    Expect(Has(json, "\"conn_id\":7"), "conn id");
    Expect(Has(json, "\"player_id\":819"), "player id");
    Expect(Has(json, "\"unsent_bytes\":21000"), "unsent bytes");
    Expect(Has(json, "\"app_bytes\":110"), "app bytes");
    Expect(Has(json, "\"tcpi_unacked\":4"), "unacked");
    Expect(Has(json, "\"tcpi_retrans\":3"), "retrans");
    Expect(Has(json, "\"tcpi_backoff\":2"), "backoff");
    Expect(Has(json, "\"tcpi_rto_us\":40000000"), "rto");
    Expect(Has(json, "\"tcpi_rtt_us\":1300000"), "rtt");
    Expect(Has(json, "\"tcpi_snd_cwnd\":2"), "cwnd");
    Expect(Has(json, "\"tcpi_last_ack_recv_ms\":5000"), "last ack");
    Expect(Has(json, "\"tcpi_last_data_sent_ms\":80"), "last data sent");
    Expect(Has(json, "\"write_fail\":1"), "write fail");
    Expect(Has(json, "\"move\":700"), "move bytes");
    Expect(Has(json, "\"reply\":200"), "reply bytes");
    Expect(Has(json, "\"enter\":50"), "enter bytes");
    Expect(Has(json, "\"leave\":30"), "leave bytes");
    Expect(Has(json, "\"other\":20"), "other bytes");
    Expect(Has(json, "\"tcpi_bytes_retrans_delta\":null"), "bytes_retrans stays null");
    Expect(Has(json, "\"notsent_bytes\":17088"), "notsent from the sample");
    Expect(Has(json, "\"notsent_source\":\"SIOCOUTQNSD\""), "notsent source");
    Expect(Has(json, "\"age_ms\":2000"), "stall age");

    std::ostringstream prom;
    AoiEvidenceWritePrometheus(prom);
    const std::string text = prom.str();
    Expect(Has(text, "gamemesh_write_fail_total{frame=\"leave\",reason=\"queue_full\"} 0"),
           "zero write_fail series is present");
    Expect(Has(text, "gamemesh_write_fail_total{frame=\"move\",reason=\"queue_full\"} 1"),
           "nonzero write_fail series");
    Expect(Has(text, "gamemesh_write_fail_per_conn_count 1"), "per-conn sample count");
    Expect(Has(text, "gamemesh_write_fail_per_conn{quantile=\"0.5\"} 1"), "per-conn p50");
    Expect(Has(text, "gamemesh_stalled_conns 1"), "stalled gauge");
    Expect(Has(text, "gamemesh_stalled_conn_total 1"), "distinct stalled connections");
    Expect(Has(text, "gamemesh_stalled_conn_age_ms_count 1"), "age sample count");
    Expect(Has(text, "gamemesh_unsent16_tcpi_backoff_count 2"), "backoff sample count");
    Expect(text.find("gamemesh_unsent16_tcpi_bytes_retrans_delta{") == std::string::npos,
           "missing bytes_retrans quantile is omitted");
    Expect(Has(text, "gamemesh_egress_wire_bytes_estimate{frame=\"move\"} 766"),
           "wire estimate adds the 66 byte header");
    Expect(Has(text, "gamemesh_tx_frames_by_frame{frame=\"move\"} 1"), "frame counter");

    AoiObserveBusySocket(7, 3, 0, 0);
    std::ostringstream cleared;
    AoiEvidenceWritePrometheus(cleared);
    const std::string after = cleared.str();
    Expect(Has(after, "gamemesh_stalled_conns 0"), "recovered connection leaves the gauge");
    Expect(Has(after, "gamemesh_stalled_conn_total 1"), "distinct total stays");
    Expect(Has(after, "gamemesh_write_fail_per_conn_count 1"), "failure count remains");

    AoiResetEvidenceForTest();
    std::ostringstream empty;
    AoiEvidenceWritePrometheus(empty);
    Expect(Has(empty.str(), "gamemesh_write_fail_total{frame=\"heartbeat\",reason=\"closed\"} 0"),
           "zero series still exported after reset");
    Expect(Has(empty.str(), "gamemesh_write_fail_per_conn_count 0"), "empty per-conn count is 0");
    Expect(empty.str().find("gamemesh_write_fail_per_conn{quantile=") == std::string::npos,
           "empty distribution has no quantile");

    if (fails != 0) {
        std::printf("%d failed\n", fails);
        return 1;
    }
    std::printf("OK stalled_conn_test\n");
    return 0;
}
