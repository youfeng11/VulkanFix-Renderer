import unittest
from verify_core11_post import report_errors


def report():
    return [
        {"name": "device", "status": "INFO"},
        {"name": "Vulkan 1.1.0 Core", "status": "PASS"},
        {"name": "core11.requiredCapabilities", "status": "PASS"},
        {"name": "core11.queryChain", "status": "PASS"},
        {"name": "core11.subgroupBasic", "status": "PASS", "evidence": "functional"},
        {"name": "core11.optional", "status": "SKIP"},
        {"name": "core11.summary", "status": "PASS"},
    ]


class ReportGateTest(unittest.TestCase):
    def test_basic_execution_cannot_be_missing_or_replaced_by_a_query(self):
        rows = report()
        self.assertTrue(report_errors(0, [row for row in rows if row["name"] != "core11.subgroupBasic"]))
        next(row for row in rows if row["name"] == "core11.subgroupBasic")["evidence"] = "query"
        self.assertTrue(report_errors(0, rows))

    def test_only_absent_compute_queues_allow_a_basic_execution_skip(self):
        rows = report()
        basic = next(row for row in rows if row["name"] == "core11.subgroupBasic")
        basic["status"] = "SKIP"
        basic["detail"] = "feature unavailable"
        self.assertTrue(report_errors(0, rows))
        basic["detail"] = "no compute queue"
        self.assertEqual(report_errors(0, rows), [])

    def test_optional_skips_do_not_fail_valid_required_gates(self):
        self.assertEqual(report_errors(0, report()), [])

    def test_summary_alone_cannot_claim_success(self):
        self.assertTrue(report_errors(0, [{"name": "core11.summary", "status": "PASS"}]))

    def test_each_mandatory_gate_must_exist_and_pass(self):
        for name in ("Vulkan 1.1.0 Core", "core11.requiredCapabilities", "core11.queryChain"):
            with self.subTest(name=name):
                rows = report()
                self.assertTrue(report_errors(0, [row for row in rows if row["name"] != name]))
                for status in ("SKIP", "FAIL"):
                    rows = report()
                    next(row for row in rows if row["name"] == name)["status"] = status
                    self.assertTrue(report_errors(0, rows))

    def test_a_second_device_cannot_hide_missing_results(self):
        rows = report()
        rows.insert(1, {"name": "device", "status": "INFO"})
        self.assertTrue(report_errors(0, rows))

    def test_failed_probe_cannot_be_hidden_by_successful_summary(self):
        rows = report()
        rows.insert(0, {"name": "core11.multiviewRenderPass", "status": "FAIL"})
        self.assertTrue(report_errors(0, rows))

    def test_process_and_summary_failures(self):
        self.assertTrue(report_errors(1, report()))
        rows = report()
        rows[-1]["status"] = "FAIL"
        self.assertTrue(report_errors(0, rows))
        self.assertTrue(report_errors(0, report()[:-1]))

    def test_inherited_core_functionality_must_not_be_hidden(self):
        rows = report()
        rows.insert(0, {"name": "fillModeNonSolid.line", "status": "FAIL", "evidence": "functional"})
        self.assertTrue(report_errors(0, rows))


if __name__ == "__main__":
    unittest.main()
