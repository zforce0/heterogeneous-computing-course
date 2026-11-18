#pragma once

/* Hungarian (Kuhn-Munkres) 最小代价分配算法, header-only 实现
 * (camerayolo.md 第 6 节 hungarian.h)。
 * 经典 O(n^3) 势函数算法, 用于 ByteTrack 两轮 IoU 匹配。 */

#include <cstddef>
#include <vector>

namespace hungarian_detail {

/* 求解 rows x cols 最小代价分配, 要求 rows <= cols
 * assignment[row] = col (-1 表示未分配) */
inline void solve_small_rows(
    const std::vector<std::vector<float> >& cost,
    int rows, int cols, std::vector<int>& assignment)
{
    const int n = rows;
    const int m = cols;
    const float INF = 1.0e18f;

    /* 内部使用 1-based 势数组 (e-maxx 风格) */
    std::vector<float> u(n + 1, 0.0f);
    std::vector<float> v(m + 1, 0.0f);
    std::vector<int> p(m + 1, 0);   /* p[j] = 分配到列 j 的行 */
    std::vector<int> way(m + 1, 0);

    for (int i = 1; i <= n; ++i) {
        p[0] = i;
        int j0 = 0;
        std::vector<float> minv(m + 1, INF);
        std::vector<char> used(m + 1, 0);

        do {
            used[j0] = 1;
            const int i0 = p[j0];
            float delta = INF;
            int j1 = -1;

            for (int j = 1; j <= m; ++j) {
                if (used[j]) {
                    continue;
                }
                const float cur =
                    cost[i0 - 1][j - 1] - u[i0] - v[j];
                if (cur < minv[j]) {
                    minv[j] = cur;
                    way[j] = j0;
                }
                if (minv[j] < delta) {
                    delta = minv[j];
                    j1 = j;
                }
            }

            for (int j = 0; j <= m; ++j) {
                if (used[j]) {
                    u[p[j]] += delta;
                    v[j] -= delta;
                } else {
                    minv[j] -= delta;
                }
            }
            j0 = j1;
        } while (p[j0] != 0);

        do {
            const int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0 != 0);
    }

    assignment.assign(n, -1);
    for (int j = 1; j <= m; ++j) {
        if (p[j] >= 1 && p[j] <= n) {
            assignment[p[j] - 1] = j - 1;
        }
    }
}

} // namespace hungarian_detail

/* 通用入口: 任意 rows x cols (内部转置保证 rows <= cols)。
 * cost[i][j] 为行 i 分配到列 j 的代价, 求总和最小的完美分配
 * (行数少于列数时所有行都被分配)。
 * assignment[i] = 分配的列, 未分配为 -1。返回总代价。 */
inline float hungarian_solve(
    const std::vector<std::vector<float> >& cost,
    std::vector<int>& assignment)
{
    if (cost.empty() || cost[0].empty()) {
        assignment.clear();
        return 0.0f;
    }
    const int rows = static_cast<int>(cost.size());
    const int cols = static_cast<int>(cost[0].size());

    if (rows <= cols) {
        hungarian_detail::solve_small_rows(cost, rows, cols,
                                           assignment);
        float total = 0.0f;
        for (int i = 0; i < rows; ++i) {
            if (assignment[i] >= 0) {
                total += cost[i][assignment[i]];
            }
        }
        return total;
    }

    /* rows > cols: 转置后求解, 再映射回来 */
    std::vector<std::vector<float> > transposed(
        cols, std::vector<float>(rows));
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            transposed[j][i] = cost[i][j];
        }
    }
    std::vector<int> col_to_row;
    hungarian_detail::solve_small_rows(transposed, cols, rows,
                                       col_to_row);
    assignment.assign(rows, -1);
    float total = 0.0f;
    for (int j = 0; j < cols; ++j) {
        if (col_to_row[j] >= 0) {
            assignment[col_to_row[j]] = j;
            total += cost[col_to_row[j]][j];
        }
    }
    return total;
}
