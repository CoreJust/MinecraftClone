#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <queue>
#include <ranges>
#include <span>
#include <vector>

namespace core {

template<typename F>
concept SuccessorProvider = true
    && std::invocable<F, size_t>
    && std::ranges::input_range<std::invoke_result_t<F, size_t>>
    && requires {
        static_cast<size_t>(std::declval<std::ranges::range_value_t<std::invoke_result_t<F, size_t>>>());
    };

/*
 * successors is a callback that returns the successors of the given node.
 * Returns a vector of node indices in the sorted_nodes order.
 * Returns nullopt if a cycle is detected.
 */
template<SuccessorProvider F>
[[nodiscard]]
std::optional<std::vector<size_t>> topoSort(size_t const node_count, F&& successors) noexcept {
    std::vector<uint32_t> in_degree(node_count, 0);
    std::vector<std::vector<size_t>> dependents(node_count);

    for (size_t i = 0; i < node_count; ++i) {
        for (auto const successor : successors(i)) {
            auto const successor_idx = static_cast<size_t>(successor);
            dependents[successor_idx].push_back(i);
            ++in_degree[successor_idx];
        }
    }

    std::queue<size_t> queue;
    for (size_t i = 0; i < node_count; ++i) {
        if (in_degree[i] == 0) {
            queue.push(i);
        }
    }

    std::vector<size_t> sorted_nodes;
    sorted_nodes.reserve(node_count);
    while (!queue.empty()) {
        size_t const node = queue.front();
        queue.pop();
        sorted_nodes.push_back(node);
        for (auto const successor : successors(node)) {
            auto const successor_idx = static_cast<size_t>(successor);
            if (--in_degree[successor_idx] == 0) {
                queue.push(successor_idx);
            }
        }
    }

    if (sorted_nodes.size() != node_count) {
        return std::nullopt;
    }
    return sorted_nodes;
}

} // namespace core
