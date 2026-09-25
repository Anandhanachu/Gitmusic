import os
import sys

# Ensure backend directory is in path
sys.path.insert(0, os.path.dirname(__file__))

from composer import compose_from_github, calculate_bpm

def run_tests():
    # Test Case 1: 1 active week -> 55 BPM, 1 phrase
    grid1 = [[0]*7 for _ in range(52)]
    grid1[10][2] = 3
    t1 = compose_from_github(grid1, username="alice")
    assert t1["bpm"] == 55, f"Expected 55 BPM, got {t1['bpm']}"
    assert t1["active_week_count"] == 1
    assert len(t1["events"]) == 1
    assert t1["events"][0]["week"] == 10 and t1["events"][0]["day"] == 2
    print("Case 1 (1 active week) PASSED: BPM =", t1["bpm"], "Events:", len(t1["events"]))

    # Test Case 2: 5 active weeks -> 67 BPM
    grid2 = [[0]*7 for _ in range(52)]
    for w in [2, 8, 14, 20, 26]:
        grid2[w][1] = 2
        grid2[w][4] = 4
    t2 = compose_from_github(grid2, username="bob")
    assert t2["bpm"] == 55 + (5 - 1) * 3  # 67 BPM
    assert t2["active_week_count"] == 5
    print("Case 2 (5 active weeks) PASSED: BPM =", t2["bpm"], "Events:", len(t2["events"]))

    # Test Case 3: All-zero profile
    grid3 = [[0]*7 for _ in range(52)]
    t3 = compose_from_github(grid3, username="charlie")
    assert t3["active_week_count"] == 0
    assert len(t3["events"]) == 0
    print("Case 3 (All zero profile) PASSED: Events =", len(t3["events"]))

    # Test Case 4: Exactly one contribution day in active week
    grid4 = [[0]*7 for _ in range(52)]
    grid4[5][3] = 7
    t4 = compose_from_github(grid4, username="david")
    assert len(t4["events"]) == 1
    assert t4["events"][0]["week"] == 5 and t4["events"][0]["day"] == 3
    print("Case 4 (Single contribution day) PASSED: day =", t4["events"][0]["day"])

    # Test Case 5: 7 contribution days (all days active) -> 1..7 selected unique days
    grid5 = [[0]*7 for _ in range(52)]
    grid5[0] = [1, 2, 3, 4, 5, 6, 7]
    t5 = compose_from_github(grid5, username="eve")
    assert 1 <= len(t5["events"]) <= 7
    selected_days = [e["day"] for e in t5["events"]]
    assert len(selected_days) == len(set(selected_days)), "Duplicate days found!"
    print("Case 5 (7 contribution days) PASSED: selected days =", selected_days)

    # Test Case 6: Mixed active and inactive weeks -> only active weeks produce music
    grid6 = [[0]*7 for _ in range(52)]
    grid6[0][1] = 1 # active
    grid6[2][2] = 2 # active
    grid6[4][3] = 3 # active
    t6 = compose_from_github(grid6, username="frank")
    event_weeks = set(e["week"] for e in t6["events"])
    assert event_weeks == {0, 2, 4}
    print("Case 6 (Mixed active/inactive weeks) PASSED: active event weeks =", event_weeks)

    # Test Case 7: Seeded determinism -> same user + data produces identical composition
    t7_a = compose_from_github(grid2, username="bob")
    t7_b = compose_from_github(grid2, username="bob")
    assert t7_a["events"] == t7_b["events"], "Deterministic seed failed!"
    print("Case 7 (Deterministic seed) PASSED: Exact match on repeated composition")

    print("\n>>> ALL TEST CASES VERIFIED AND PASSED! <<<")

if __name__ == "__main__":
    run_tests()
