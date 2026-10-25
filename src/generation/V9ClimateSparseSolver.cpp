#include "V9ClimateSparseSolver.hpp"

#include "slu_ddefs.h"

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>

namespace generation::detail
{
namespace
{
void checkCancellation(std::stop_token cancellation)
{
    if (cancellation.stop_requested())
        throw std::runtime_error("V9 sparse thermal solve cancelled");
}

struct SuperLuResources final
{
    SuperMatrix matrix{};
    SuperMatrix lower{};
    SuperMatrix upper{};
    SuperMatrix right_hand_side{};
    SuperLUStat_t statistics{};
    bool statistics_initialized = false;

    ~SuperLuResources()
    {
        if (right_hand_side.Store)
            Destroy_SuperMatrix_Store(&right_hand_side);
        if (matrix.Store)
            Destroy_SuperMatrix_Store(&matrix);
        if (lower.Store)
            Destroy_SuperNode_Matrix(&lower);
        if (upper.Store)
            Destroy_CompCol_Matrix(&upper);
        if (statistics_initialized)
            StatFree(&statistics);
    }
};
} // namespace

struct V9ClimateSparseLu::Impl final
{
    explicit Impl(std::span<const Row> rows, std::stop_token cancellation)
    {
        checkCancellation(cancellation);
        if (rows.empty() || rows.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
            throw std::invalid_argument("V9 sparse thermal matrix has an invalid dimension");

        dimension = static_cast<int>(rows.size());
        column_offsets.resize(static_cast<size_t>(dimension) + 1);
        for (Row const &row : rows)
        {
            for (auto const &[column, value] : row)
            {
                static_cast<void>(value);
                if (column < 0 || column >= dimension)
                    throw std::invalid_argument("V9 sparse thermal matrix has an invalid column");
                if (column_offsets[static_cast<size_t>(column) + 1] == std::numeric_limits<int_t>::max())
                    throw std::length_error("V9 sparse thermal matrix exceeds SuperLU index capacity");
                ++column_offsets[static_cast<size_t>(column) + 1];
            }
        }
        for (int column = 0; column < dimension; ++column)
        {
            int_t const previous = column_offsets[static_cast<size_t>(column)];
            int_t const count = column_offsets[static_cast<size_t>(column) + 1];
            if (count > std::numeric_limits<int_t>::max() - previous)
                throw std::length_error("V9 sparse thermal matrix exceeds SuperLU index capacity");
            column_offsets[static_cast<size_t>(column) + 1] = previous + count;
        }

        row_indices.resize(static_cast<size_t>(column_offsets.back()));
        values.resize(row_indices.size());
        auto next = column_offsets;
        for (int row_index = 0; row_index < dimension; ++row_index)
        {
            checkCancellation(cancellation);
            for (auto const &[column, value] : rows[static_cast<size_t>(row_index)])
            {
                size_t const position = static_cast<size_t>(next[static_cast<size_t>(column)]++);
                row_indices[position] = static_cast<int_t>(row_index);
                values[position] = value;
            }
        }

        column_permutation.resize(static_cast<size_t>(dimension));
        row_permutation.resize(static_cast<size_t>(dimension));
        dCreate_CompCol_Matrix(&resources.matrix, dimension, dimension,
                               static_cast<int_t>(values.size()), values.data(),
                               row_indices.data(), column_offsets.data(), SLU_NC, SLU_D, SLU_GE);

        std::vector<double> zero_rhs(static_cast<size_t>(dimension));
        dCreate_Dense_Matrix(&resources.right_hand_side, dimension, 1, zero_rhs.data(),
                             dimension, SLU_DN, SLU_D, SLU_GE);
        StatInit(&resources.statistics);
        resources.statistics_initialized = true;

        superlu_options_t options;
        set_default_options(&options);
        options.ColPerm = COLAMD;
        options.PrintStat = NO;

        int_t info = 0;
        dgssv(&options, &resources.matrix, column_permutation.data(), row_permutation.data(),
              &resources.lower, &resources.upper, &resources.right_hand_side,
              &resources.statistics, &info);
        if (info != 0)
        {
            if (info <= dimension)
                throw std::runtime_error("V9 sparse thermal saddle system is singular");
            throw std::runtime_error("SuperLU failed to allocate a V9 thermal factorization");
        }
        checkCancellation(cancellation);
    }

    std::vector<double> solve(std::span<const double> rhs, std::stop_token cancellation)
    {
        checkCancellation(cancellation);
        if (rhs.size() != static_cast<size_t>(dimension))
            throw std::invalid_argument("V9 sparse thermal right-hand side has an invalid size");

        std::vector<double> solution(rhs.begin(), rhs.end());
        auto *dense_rhs = static_cast<DNformat *>(resources.right_hand_side.Store);
        dense_rhs->nzval = solution.data();
        int info = 0;
        dgstrs(NOTRANS, &resources.lower, &resources.upper, column_permutation.data(),
               row_permutation.data(), &resources.right_hand_side, &resources.statistics, &info);
        if (info != 0)
            throw std::runtime_error("SuperLU failed to solve a V9 thermal correction system");
        checkCancellation(cancellation);
        return solution;
    }

    int dimension = 0;
    std::vector<int_t> column_offsets;
    std::vector<int_t> row_indices;
    std::vector<double> values;
    std::vector<int> column_permutation;
    std::vector<int> row_permutation;
    SuperLuResources resources;
};

V9ClimateSparseLu::V9ClimateSparseLu(std::span<const Row> rows,
                            std::stop_token cancellation)
    : impl_(std::make_unique<Impl>(rows, cancellation))
{
}

V9ClimateSparseLu::~V9ClimateSparseLu() = default;
V9ClimateSparseLu::V9ClimateSparseLu(V9ClimateSparseLu &&) noexcept = default;
V9ClimateSparseLu &V9ClimateSparseLu::operator=(V9ClimateSparseLu &&) noexcept = default;

std::vector<double> V9ClimateSparseLu::solve(std::span<const double> rhs,
                            std::stop_token cancellation)
{
    return impl_->solve(rhs, cancellation);
}
} // namespace generation::detail
