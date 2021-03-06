// Copyright 2018, Intel Corp.

#include <gmock/gmock.h>
#include <google/protobuf/util/json_util.h>

#include "tile/codegen/tile.h"
#include "tile/codegen/vm.h"
#include "tile/lang/compose.h"
#include "tile/lang/gen_stripe.h"
#include "tile/lib/lib.h"
#include "tile/stripe/stripe.h"

using ::testing::ContainerEq;
using ::testing::Eq;

namespace vertexai {
namespace tile {
namespace codegen {
namespace test {

namespace {

template <typename T>
T ParseProtoJson(const std::string& str) {
  T proto;
  google::protobuf::util::JsonStringToMessage(str, &proto);
  return proto;
}

}  // namespace

TEST(Codegen, ApplyTile) {
  std::map<std::string, std::vector<float>> data = {
      {"A",
       {
           1, 2, 3, 4, 5,  //
           4, 5, 6, 7, 8,  //
           7, 8, 9, 7, 8,  //
           1, 2, 3, 1, 2,  //
           1, 2, 3, 1, 2,  //
       }},
      {"B",
       {
           1, 2, 3, 1, 2,  //
           1, 2, 3, 1, 2,  //
           1, 2, 3, 1, 2,  //
           1, 2, 3, 1, 2,  //
           1, 2, 3, 1, 2,  //
       }},
      {"C",
       {
           0, 0, 0, 0, 0,  //
           0, 0, 0, 0, 0,  //
           0, 0, 0, 0, 0,  //
           0, 0, 0, 0, 0,  //
           0, 0, 0, 0, 0,  //
       }},
  };

  std::vector<float> expected = {
      15, 30, 45,  15, 30,  //
      30, 60, 90,  30, 60,  //
      39, 78, 117, 39, 78,  //
      9,  18, 27,  9,  18,  //
      9,  18, 27,  9,  18,  //
  };

  size_t dim = sqrt(expected.size());
  auto runinfo = lib::LoadMatMul("matmul",                                    //
                                 SimpleShape(DataType::FLOAT32, {dim, dim}),  //
                                 SimpleShape(DataType::FLOAT32, {dim, dim}));
  auto program = GenerateStripe(runinfo);
  auto main = program->SubBlock(0);

  IVLOG(2, "Before>\n" << *program);

  ExecuteProgram(*program, &data);

  IVLOG(2, "A: " << data["A"]);
  IVLOG(2, "B: " << data["B"]);
  IVLOG(2, "C: " << data["C"]);
  EXPECT_THAT(data["C"], ContainerEq(expected));

  for (size_t i = 0; i < data["C"].size(); i++) {
    data["C"][i] = 0;
  }

  auto kernel = main->SubBlock(0);
  ApplyTile(kernel.get(), {5, 2, 2});
  auto inner = kernel->SubBlock(0);
  inner->name = "inner";
  ApplyTile(inner.get(), {5, 3, 2, 1, 1});
  auto innermost = inner->SubBlock(0);
  innermost->name = "innermost";
  ApplyTile(kernel.get(), {5, 2, 2});
  kernel->SubBlock(0)->name = "outer";

  IVLOG(2, "After>\n" << *program);

  ExecuteProgram(*program, &data);

  IVLOG(2, "A: " << data["A"]);
  IVLOG(2, "B: " << data["B"]);
  IVLOG(2, "C: " << data["C"]);
  EXPECT_THAT(data["C"], ContainerEq(expected));
}

TEST(Codegen, StencilMatchMatMul) {
  auto spec = ParseProtoJson<proto::Stencil>(R"(
    {
      "startup_cost": 32,
      "idxs": [
        { "name": "k", "size": 16, "outs": [ -1 ], "ins": [ -1,  0 ] },
        { "name": "x", "size": 16, "outs": [ -1 ], "ins": [  0, -1 ] },
        { "name": "c", "size": -1, "outs": [  0 ], "ins": [ -1, -1 ] },
      ]
    }
  )");

  const auto DIM = 100;
  auto runinfo = lib::LoadMatMul("matmul",                                    //
                                 SimpleShape(DataType::FLOAT32, {DIM, DIM}),  //
                                 SimpleShape(DataType::FLOAT32, {DIM, DIM}));
  auto program = GenerateStripe(runinfo);
  auto main = program->SubBlock(0);
  auto kernel = main->SubBlock(0);

  IVLOG(2, *kernel);

  auto match = FindBestStencil({spec}, *kernel);
  IVLOG(1, "Best match: " << *match);
  StencilMatch expected{
      1255968,  // total
      {
          {"k", "c", 100},
          {"m", "k", 16},
          {"n", "x", 16},
      }  // idxs
  };
  EXPECT_THAT(*match, Eq(expected));
}

TEST(Codegen, StencilMatchConv1D) {
  auto spec = ParseProtoJson<proto::Stencil>(R"(
    {
      "startup_cost": 32,
      "idxs": [
        { "name": "k", "size": 16, "outs": [ -1 ], "ins": [ -1,  0 ] },
        { "name": "x", "size": 16, "outs": [ -1 ], "ins": [  0, -1 ] },
        { "name": "c", "size": -1, "outs": [  0 ], "ins": [ -1, -1 ] },
      ]
    }
  )");

  auto runinfo = lib::LoadConv1d("conv",                                       //
                                 SimpleShape(DataType::FLOAT32, {100, 64}),    //
                                 SimpleShape(DataType::FLOAT32, {3, 64, 64}),  //
                                 SimpleShape(DataType::FLOAT32, {100, 64}));
  auto program = GenerateStripe(runinfo);
  auto main = program->SubBlock(0);
  auto kernel = main->SubBlock(0);

  IVLOG(2, *kernel);

  auto match = FindBestStencil({spec}, *kernel);
  IVLOG(1, "Best match: " << *match);
  StencilMatch expected{
      1378944,  // total
      {
          {"ci", "c", 64},
          {"co", "x", 16},
          {"kx", "*", 1},
          {"x", "k", 16},
      }  // idxs
  };
  EXPECT_THAT(*match, Eq(expected));
}

TEST(Codegen, StencilMatchConv2D) {
  auto options = ParseProtoJson<proto::StencilPass>(R"(
    {
      "stencils": [
        {
          "startup_cost": 32,
          "idxs": [
            { "name": "k", "size": 16, "outs": [ -1 ], "ins": [ -1,  0 ] },
            { "name": "x", "size": 16, "outs": [ -1 ], "ins": [  0, -1 ] },
            { "name": "c", "size": -1, "outs": [  0 ], "ins": [ -1, -1 ] },
          ]
        },
        {
          "startup_cost": 32,
          "idxs": [
            { "name": "k", "size": 16, "outs": [ -1 ], "ins": [ -1,  0 ] },
            { "name": "x", "size":  4, "outs": [ -1 ], "ins": [  0, -1 ] },
            { "name": "y", "size":  4, "outs": [ -1 ], "ins": [  0, -1 ] },
            { "name": "c", "size": -1, "outs": [  0 ], "ins": [ -1, -1 ] },
          ]
        },
        {
          "startup_cost": 32,
          "idxs": [
            { "name": "k", "size": 16, "outs": [ -1 ], "ins": [  0, -1 ] },
            { "name": "x", "size":  4, "outs": [ -1 ], "ins": [ -1,  0 ] },
            { "name": "y", "size":  4, "outs": [ -1 ], "ins": [ -1,  0 ] },
            { "name": "c", "size": -1, "outs": [  0 ], "ins": [ -1, -1 ] },
          ]
        }
      ]
    }
  )");
  std::vector<proto::Stencil> specs;
  for (const auto& stencil : options.stencils()) {
    specs.push_back(stencil);
  }

  auto runinfo = lib::LoadConv2d("conv",                                           //
                                 SimpleShape(DataType::FLOAT32, {100, 100, 56}),   //
                                 SimpleShape(DataType::FLOAT32, {3, 3, 56, 56}),   //
                                 SimpleShape(DataType::FLOAT32, {100, 100, 56}));  //
  auto program = GenerateStripe(runinfo);
  auto main = program->SubBlock(0);
  auto kernel = main->SubBlock(0);

  IVLOG(2, "\n" << *kernel);

  auto match = FindBestStencil(specs, *kernel);
  IVLOG(1, "Best match: " << *match);
  StencilMatch expected{
      323280000,  // total
      {
          {"ci", "c", 56},
          {"co", "k", 16},
          {"kx", "*", 1},
          {"ky", "*", 1},
          {"x", "x", 4},
          {"y", "y", 4},
      }  // idxs
  };
  EXPECT_THAT(*match, Eq(expected));
}

TEST(Codegen, StencilPass) {
  std::map<std::string, std::vector<float>> data = {
      {"A",
       {
           1, 2, 3, 4, 5,  //
           4, 5, 6, 7, 8,  //
           7, 8, 9, 7, 8,  //
           1, 2, 3, 1, 2,  //
           1, 2, 3, 1, 2,  //
       }},
      {"B",
       {
           1, 2, 3, 1, 2,  //
           1, 2, 3, 1, 2,  //
           1, 2, 3, 1, 2,  //
           1, 2, 3, 1, 2,  //
           1, 2, 3, 1, 2,  //
       }},
      {"C",
       {
           0, 0, 0, 0, 0,  //
           0, 0, 0, 0, 0,  //
           0, 0, 0, 0, 0,  //
           0, 0, 0, 0, 0,  //
           0, 0, 0, 0, 0,  //
       }},
  };

  std::vector<float> expected = {
      15, 30, 45,  15, 30,  //
      30, 60, 90,  30, 60,  //
      39, 78, 117, 39, 78,  //
      9,  18, 27,  9,  18,  //
      9,  18, 27,  9,  18,  //
  };

  auto options = ParseProtoJson<proto::StencilPass>(R"(
    {
      "reqs": ["agg_op_add", "comb_op_mul"],
      "startup_cost": 32,
      "idxs": [
        { "name": "k", "size":  2, "outs": [ -1 ], "ins": [ -1,  0 ] },
        { "name": "x", "size":  2, "outs": [ -1 ], "ins": [  0, -1 ] },
        { "name": "c", "size": -1, "outs": [  0 ], "ins": [ -1, -1 ] },
      ]
    }
  )");

  const auto DIM = 5;
  auto runinfo = lib::LoadMatMul("matmul",                                    //
                                 SimpleShape(DataType::FLOAT32, {DIM, DIM}),  //
                                 SimpleShape(DataType::FLOAT32, {DIM, DIM}));
  auto program = GenerateStripe(runinfo);
  StencilPass(program.get(), options);
  IVLOG(2, "\n" << *program);

  ExecuteProgram(*program, &data);

  IVLOG(2, "A: " << data["A"]);
  IVLOG(2, "B: " << data["B"]);
  IVLOG(2, "C: " << data["C"]);
  EXPECT_THAT(data["C"], ContainerEq(expected));
}

TEST(Codegen, TilePassBroadcast) {
  lang::RunInfo runinfo;
  runinfo.program_name = "broadcast";
  runinfo.code = "function (A, B) -> (C) { C = add(A, B); }";
  runinfo.input_shapes.emplace("A", SimpleShape(DataType::FLOAT32, {1, 112, 112, 32}));
  runinfo.input_shapes.emplace("B", SimpleShape(DataType::FLOAT32, {32}));
  runinfo.output_shapes.emplace("C", SimpleShape(DataType::FLOAT32, {1, 112, 112, 32}));
  auto program = GenerateStripe(runinfo);
  auto main = program->SubBlock(0);

  IVLOG(2, "\n" << *program);
}

}  // namespace test
}  // namespace codegen
}  // namespace tile
}  // namespace vertexai
