"""Paired rates must compare the two settings, and refuse a missing one."""

from __future__ import annotations

import pytest
from wiiuport.paired import PairedRates


def test_each_setting_is_the_sum_of_its_own_windows():
    rates = PairedRates()
    rates.add(False, 90, 3.0)
    rates.add(True, 60, 3.0)
    rates.add(False, 60, 2.0)
    assert rates.rate(False) == 30.0
    assert rates.rate(True) == 20.0
    assert "20.00 Hz with interpolation on, 30.00 Hz off (66.7%)" in rates.render()


def test_a_setting_never_measured_is_refused():
    rates = PairedRates()
    rates.add(False, 90, 3.0)
    with pytest.raises(ValueError, match="on"):
        rates.render()


def test_an_empty_window_is_refused():
    with pytest.raises(ValueError, match="not a measurement"):
        PairedRates().add(True, 10, 0.0)
