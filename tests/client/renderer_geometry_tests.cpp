#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

[[nodiscard]] std::string readShader(char const* const name)
{
    std::ifstream input{ std::string{ MC_SHADER_SOURCE_DIR } + "/" + name };
    EXPECT_TRUE(input.good()) << "unable to read shader source " << name;
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

[[nodiscard]] std::vector<glm::vec3> parseVertices(std::string const& source, size_t const expected_count)
{
    std::regex const pattern{ R"(vec3\(\s*([01])\s*,\s*([01])\s*,\s*([01])\s*\))" };
    std::vector<glm::vec3> vertices;
    for (std::sregex_iterator iterator{ source.begin(), source.end(), pattern }, end;
         iterator != end;
         ++iterator) {
        vertices.emplace_back(
            std::stof((*iterator)[1].str()),
            std::stof((*iterator)[2].str()),
            std::stof((*iterator)[3].str())
        );
    }
    EXPECT_EQ(vertices.size(), expected_count);
    return vertices;
}

[[nodiscard]] std::vector<std::array<uint32_t, 3>> parseTriangles(std::string const& source)
{
    std::regex const pattern{ R"(uvec3\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\))" };
    std::vector<std::array<uint32_t, 3>> triangles;
    for (std::sregex_iterator iterator{ source.begin(), source.end(), pattern }, end;
         iterator != end;
         ++iterator) {
        triangles.push_back({
            static_cast<uint32_t>(std::stoul((*iterator)[1].str())),
            static_cast<uint32_t>(std::stoul((*iterator)[2].str())),
            static_cast<uint32_t>(std::stoul((*iterator)[3].str())),
        });
    }
    return triangles;
}

TEST(RendererGeometryTest, BothPlayerVariantsUseOutwardCubeWinding)
{
    std::string const vertex_source = readShader("player.vert");
    std::string const mesh_source = readShader("player.mesh");
    std::vector<glm::vec3> const vertex_vertices = parseVertices(vertex_source, 36U);
    std::vector<glm::vec3> const mesh_vertices = parseVertices(mesh_source, 8U);
    glm::vec3 const cube_center{ 0.5F };
    for (uint32_t index = 0U; index < vertex_vertices.size(); index += 3U) {
        glm::vec3 const first = vertex_vertices[index];
        glm::vec3 const second = vertex_vertices[index + 1U];
        glm::vec3 const third = vertex_vertices[index + 2U];
        EXPECT_GT(glm::dot(glm::cross(second - first, third - first),
                           ((first + second + third) / 3.0F) - cube_center), 0.0F);
    }
    std::vector<std::array<uint32_t, 3>> const triangles = parseTriangles(mesh_source);
    ASSERT_EQ(triangles.size(), 12U);
    for (std::array<uint32_t, 3> const triangle : triangles) {
        glm::vec3 const first = mesh_vertices[triangle[0]];
        glm::vec3 const second = mesh_vertices[triangle[1]];
        glm::vec3 const third = mesh_vertices[triangle[2]];
        EXPECT_GT(glm::dot(glm::cross(second - first, third - first),
                           ((first + second + third) / 3.0F) - cube_center), 0.0F);
    }
}

TEST(RendererGeometryTest, GridVariantsUseMatchingFrontFacingWinding)
{
    std::string const vertex_source = readShader("grid.vert");
    std::string const mesh_source = readShader("grid.mesh");
    std::regex const indices_pattern{ R"(kIndices\[6\]\s*=\s*uint\[\]\(([^;]+)\))" };
    std::smatch indices_match;
    ASSERT_TRUE(std::regex_search(vertex_source, indices_match, indices_pattern));
    std::regex const index_pattern{ R"((\d+)u)" };
    std::vector<uint32_t> indices;
    std::string const index_source = indices_match[1].str();
    for (std::sregex_iterator iterator{ index_source.begin(), index_source.end(), index_pattern }, end;
         iterator != end;
         ++iterator) {
        indices.push_back(static_cast<uint32_t>(std::stoul((*iterator)[1].str())));
    }
    ASSERT_EQ(indices.size(), 6U);
    std::array<glm::vec2, 4> const corners{
        glm::vec2{ 0.0F, 0.0F }, glm::vec2{ 1.0F, 0.0F },
        glm::vec2{ 0.0F, 1.0F }, glm::vec2{ 1.0F, 1.0F },
    };
    auto orientation = [&corners](std::array<uint32_t, 3> const triangle) {
        glm::vec2 const first_edge = corners[triangle[1]] - corners[triangle[0]];
        glm::vec2 const second_edge = corners[triangle[2]] - corners[triangle[0]];
        return first_edge.x * second_edge.y - first_edge.y * second_edge.x;
    };
    std::vector<std::array<uint32_t, 3>> const mesh_triangles = parseTriangles(mesh_source);
    ASSERT_EQ(mesh_triangles.size(), 2U);
    EXPECT_GT(orientation({ indices[0], indices[1], indices[2] }), 0.0F);
    EXPECT_GT(orientation({ indices[3], indices[4], indices[5] }), 0.0F);
    EXPECT_GT(orientation(mesh_triangles[0]), 0.0F);
    EXPECT_GT(orientation(mesh_triangles[1]), 0.0F);
}

} // namespace
