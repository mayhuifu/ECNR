#!/usr/bin/env python3
"""Unit tests for check_gbt45314_ecall_gate.py."""

from __future__ import annotations

import unittest

import check_gbt45314_ecall_gate as gate


def passing_row(condition_id: str = "ecall_farend_quiet_tcl_convergence") -> dict[str, str]:
    return {
        "condition_id": condition_id,
        "frame_duration_ms": "10",
        "rtf": "0.05",
        "erle_true_median_db": "55",
        "erle_initial_0ms_db": "5",
        "erle_initial_200ms_db": "6",
        "erle_initial_1000ms_db": "22",
        "erle_initial_1200ms_db": "42",
        "erle_initial_1500ms_db": "43",
        "erle_initial_5000ms_db": "50",
        "erle_steady_median_db": "56",
        "erle_time_variation_db": "3",
        "noise_level_range_db": "3",
        "near_end_level_delta_median_db": "-3",
        "near_end_correlation": "0.80",
    }


class GateTest(unittest.TestCase):
    def test_farend_row_passes_core_echo_requirements(self) -> None:
        fails, warns = gate.grade_row(passing_row())
        self.assertEqual([], fails)
        self.assertEqual([], warns)

    def test_convergence_floor_blocks_release(self) -> None:
        row = passing_row()
        row["erle_initial_1200ms_db"] = "32"
        fails, _ = gate.grade_row(row)
        self.assertTrue(any("erle_initial_1200ms_db" in f for f in fails))

    def test_noise_only_uses_noise_stability_not_erle(self) -> None:
        row = passing_row("ecall_b2_noise_only_stability")
        row["erle_true_median_db"] = ""
        row["noise_level_range_db"] = "9.9"
        fails, _ = gate.grade_row(row)
        self.assertEqual([], fails)

    def test_noise_only_blocks_at_ten_db_variation(self) -> None:
        row = passing_row("ecall_b2_noise_only_stability")
        row["erle_true_median_db"] = ""
        row["noise_level_range_db"] = "10.0"
        fails, _ = gate.grade_row(row)
        self.assertTrue(any("noise_level_range_db" in f for f in fails))

    def test_doubletalk_blocks_near_end_damage(self) -> None:
        row = passing_row("ecall_doubletalk_driver_minus6")
        row["near_end_level_delta_median_db"] = "-18"
        row["near_end_correlation"] = "0.40"
        fails, _ = gate.grade_row(row)
        self.assertTrue(any("near_end_level_delta_median_db" in f for f in fails))
        self.assertTrue(any("near_end_correlation" in f for f in fails))

    def test_time_varying_path_blocks_degradation_above_six_db(self) -> None:
        row = passing_row("ecall_timevarying_path")
        row["erle_time_variation_db"] = "6.1"
        fails, _ = gate.grade_row(row)
        self.assertTrue(any("erle_time_variation_db" in f for f in fails))


if __name__ == "__main__":
    unittest.main()
