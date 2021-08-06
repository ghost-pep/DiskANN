// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include <omp.h>
#include <string>
#include <memory>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>
#include <pybind11/operators.h>

#include "linux_aligned_file_reader.h"
#include "aux_utils.h"
#include "pq_flash_index.h"

PYBIND11_MAKE_OPAQUE(std::vector<unsigned>);
PYBIND11_MAKE_OPAQUE(std::vector<float>);

namespace py = pybind11;
using namespace diskann;

#ifdef __linux__
template<class T>
struct DiskANNIndex {
  PQFlashIndex<T> *                  pq_flash_index;
  std::shared_ptr<AlignedFileReader> reader;

  DiskANNIndex() {
    reader = std::make_shared<LinuxAlignedFileReader>();
    pq_flash_index = new PQFlashIndex<T>(reader);
  }

  ~DiskANNIndex() {
    delete pq_flash_index;
  }
};
#endif

PYBIND11_MODULE(diskannpy, m) {
  m.doc() = "DiskANN Python Bindings";
  m.attr("__version__") = "0.1.0";

  py::bind_vector<std::vector<unsigned>>(m, "VectorUnsigned");
  py::bind_vector<std::vector<float>>(m, "VectorFloat");

  py::enum_<Metric>(m, "Metric").value("L2", Metric::L2).export_values();

  py::class_<Parameters>(m, "Parameters")
      .def(py::init<>())
      .def(
          "set",
          [](Parameters &self, const std::string &name, py::object value) {
            if (py::isinstance<py::bool_>(value)) {
              return self.Set(name, py::cast<bool>(value));
            } else if (py::isinstance<py::int_>(value)) {
              return self.Set(name, py::cast<unsigned>(value));
            } else if (py::isinstance<py::float_>(value)) {
              return self.Set(name, py::cast<float>(value));
            }
          },
          py::arg("name"), py::arg("value"));

  py::class_<Neighbor>(m, "Neighbor")
      .def(py::init<>())
      .def(py::init<unsigned, float, bool>())
      .def(py::self < py::self)
      .def(py::self == py::self);

  py::class_<SimpleNeighbor>(m, "SimpleNeighbor")
      .def(py::init<>())
      .def(py::init<unsigned, float>())
      .def(py::self < py::self)
      .def(py::self == py::self);

  py::class_<AlignedFileReader>(m, "AlignedFileReader");

  py::class_<LinuxAlignedFileReader>(m, "LinuxAlignedFileReader")
      .def(py::init<>());

  m.def(
      "omp_set_num_threads",
      [](const size_t num_threads) { omp_set_num_threads(num_threads); },
      py::arg("num_threads") = 1);

  m.def("omp_get_max_threads", []() { return omp_get_max_threads(); });

  m.def(
      "load_aligned_bin_float",
      [](const std::string &path, std::vector<float> &data) {
        float *data_ptr = nullptr;
        size_t num, dims, aligned_dims;
        load_aligned_bin<float>(path, data_ptr, num, dims, aligned_dims);
        // TODO: Remove redundant copy.
        data.assign(data_ptr, data_ptr + num * dims);
        auto l = py::list(3);
        l[0] = py::int_(num);
        l[1] = py::int_(dims);
        l[2] = py::int_(aligned_dims);
        aligned_free(data_ptr);
        return l;
      },
      py::arg("path"), py::arg("data"));

  m.def(
      "load_truthset",
      [](const std::string &path, std::vector<unsigned> &ids,
         std::vector<float> &distances) {
        unsigned *id_ptr = nullptr;
        float *   dist_ptr = nullptr;
        size_t    num, dims;
        load_truthset(path, id_ptr, dist_ptr, num, dims);
        // TODO: Remove redundant copies.
        ids.assign(id_ptr, id_ptr + num * dims);
        distances.assign(dist_ptr, dist_ptr + num * dims);
        auto l = py::list(2);
        l[0] = py::int_(num);
        l[1] = py::int_(dims);
        delete[] id_ptr;
        delete[] dist_ptr;
        return l;
      },
      py::arg("path"), py::arg("ids"), py::arg("distances"));

  m.def(
      "calculate_recall",
      [](const unsigned num_queries, std::vector<unsigned> &ground_truth_ids,
         std::vector<float> &ground_truth_dists,
         const unsigned ground_truth_dims, std::vector<unsigned> &results,
         const unsigned result_dims, const unsigned recall_at) {
        unsigned *gti_ptr = ground_truth_ids.data();
        float *   gtd_ptr = ground_truth_dists.data();
        unsigned *r_ptr = results.data();

        double             total_recall = 0;
        std::set<unsigned> gt, res;
        for (size_t i = 0; i < num_queries; i++) {
          gt.clear();
          res.clear();
          size_t tie_breaker = recall_at;
          if (gtd_ptr != nullptr) {
            tie_breaker = recall_at - 1;
            float *gt_dist_vec = gtd_ptr + ground_truth_dims * i;
            while (tie_breaker < ground_truth_dims &&
                   gt_dist_vec[tie_breaker] == gt_dist_vec[recall_at - 1])
              tie_breaker++;
          }

          gt.insert(gti_ptr + ground_truth_dims * i,
                    gti_ptr + ground_truth_dims * i + tie_breaker);
          res.insert(r_ptr + result_dims * i,
                     r_ptr + result_dims * i + recall_at);
          unsigned cur_recall = 0;
          for (auto &v : gt) {
            if (res.find(v) != res.end()) {
              cur_recall++;
            }
          }
          total_recall += cur_recall;
        }
        return py::float_(total_recall / (num_queries) * (100.0 / recall_at));
      },
      py::arg("num_queries"), py::arg("ground_truth_ids"),
      py::arg("ground_truth_dists"), py::arg("ground_truth_dims"),
      py::arg("results"), py::arg("result_dims"), py::arg("recall_at"));

  m.def(
      "calculate_recall_numpy_input",
      [](const unsigned num_queries, std::vector<unsigned> &ground_truth_ids,
         std::vector<float> &ground_truth_dists,
         const unsigned      ground_truth_dims,
         py::array_t<unsigned, py::array::c_style | py::array::forcecast>
             &          results,
         const unsigned result_dims, const unsigned recall_at) {
        unsigned *gti_ptr = ground_truth_ids.data();
        float *   gtd_ptr = ground_truth_dists.data();
        unsigned *r_ptr = results.mutable_data();

        double             total_recall = 0;
        std::set<unsigned> gt, res;
        for (size_t i = 0; i < num_queries; i++) {
          gt.clear();
          res.clear();
          size_t tie_breaker = recall_at;
          if (gtd_ptr != nullptr) {
            tie_breaker = recall_at - 1;
            float *gt_dist_vec = gtd_ptr + ground_truth_dims * i;
            while (tie_breaker < ground_truth_dims &&
                   gt_dist_vec[tie_breaker] == gt_dist_vec[recall_at - 1])
              tie_breaker++;
          }

          gt.insert(gti_ptr + ground_truth_dims * i,
                    gti_ptr + ground_truth_dims * i + tie_breaker);
          res.insert(r_ptr + result_dims * i,
                     r_ptr + result_dims * i + recall_at);
          unsigned cur_recall = 0;
          for (auto &v : gt) {
            if (res.find(v) != res.end()) {
              cur_recall++;
            }
          }
          total_recall += cur_recall;
        }
        return py::float_(total_recall / (num_queries) * (100.0 / recall_at));
      },
      py::arg("num_queries"), py::arg("ground_truth_ids"),
      py::arg("ground_truth_dists"), py::arg("ground_truth_dims"),
      py::arg("results"), py::arg("result_dims"), py::arg("recall_at"));

  m.def(
      "save_bin_u32",
      [](const std::string &file_name, std::vector<unsigned> &data, size_t npts,
         size_t dims) { save_bin<_u32>(file_name, data.data(), npts, dims); },
      py::arg("file_name"), py::arg("data"), py::arg("npts"), py::arg("dims"));

  py::class_<DiskANNIndex<float>>(m, "DiskANNFloatIndex")
      .def(py::init([]() { return new DiskANNIndex<float>(); }))
      .def(
          "load_index",
          [](DiskANNIndex<float> &self, const std::string &index_path_prefix,
             const int num_threads) {
            const std::string pq_path = index_path_prefix;
            const std::string index_path =
                index_path_prefix + std::string("_disk.index");
            int load_success = self.pq_flash_index->load(
                num_threads, pq_path.c_str(), index_path.c_str());
            if (load_success != 0) {
              std::cout << "Index load failed" << std::endl;
              return load_success;
            }
            std::vector<uint32_t> node_list;
            _u64                  num_nodes_to_cache = 1000;
            self.pq_flash_index->cache_bfs_levels(num_nodes_to_cache,
                                                  node_list);
            std::cout << "loaded index, cached " << node_list.size()
                      << " nodes based on BFS" << std::endl;
            return 0;
          },
          py::arg("index_path_prefix"), py::arg("num_threads"))
      .def(
          "search",
          [](DiskANNIndex<float> &self, std::vector<float> &query,
             const _u64 query_idx, const _u64 dim, const _u64 num_queries,
             const _u64 knn, const _u64 l_search, const _u64 beam_width,
             std::vector<unsigned> &ids, std::vector<float> &dists) {
            QueryStats stats;
            if (ids.size() < knn * num_queries) {
              ids.resize(knn * num_queries);
              dists.resize(knn * num_queries);
            }
            std::vector<_u64> _u64_ids(knn);
            self.pq_flash_index->cached_beam_search(
                query.data() + (query_idx * dim), knn, l_search,
                _u64_ids.data(), dists.data() + (query_idx * knn), beam_width,
                &stats);
            for (_u64 i = 0; i < knn; i++)
              ids[(query_idx * knn) + i] = _u64_ids[i];
          },
          py::arg("query"), py::arg("query_idx"), py::arg("dim"),
          py::arg("num_queries"), py::arg("knn"), py::arg("l_search"),
          py::arg("beam_width"), py::arg("ids"), py::arg("dists"))
      .def(
          "batch_search",
          [](DiskANNIndex<float> &self, std::vector<float> &queries,
             const _u64 dim, const _u64 num_queries, const _u64 knn,
             const _u64 l_search, const _u64 beam_width,
             std::vector<unsigned> &ids, std::vector<float> &dists,
             const int num_threads) {
            if (ids.size() < knn * num_queries) {
              ids.resize(knn * num_queries);
              dists.resize(knn * num_queries);
            }
            omp_set_num_threads(num_threads);
#pragma omp parallel for schedule(dynamic, 1)
            for (_u64 q = 0; q < num_queries; ++q) {
              std::vector<_u64> u64_ids(knn);

              self.pq_flash_index->cached_beam_search(
                  queries.data() + q * dim, knn, l_search, u64_ids.data(),
                  dists.data() + q * knn, beam_width);
              for (_u64 i = 0; i < knn; i++)
                ids[(q * knn) + i] = u64_ids[i];
            }
          },
          py::arg("queries"), py::arg("dim"), py::arg("num_queries"),
          py::arg("knn"), py::arg("l_search"), py::arg("beam_width"),
          py::arg("ids"), py::arg("dists"), py::arg("num_threads"))
      .def(
          "build",
          [](DiskANNIndex<float> &self, const char *data_file_path,
             const char *index_prefix_path, unsigned R, unsigned L,
             double final_index_ram_limit, double indexing_ram_budget,
             unsigned num_threads) {
            std::string params = std::to_string(R) + " " + std::to_string(L) +
                                 " " + std::to_string(final_index_ram_limit) +
                                 " " + std::to_string(indexing_ram_budget) +
                                 " " + std::to_string(num_threads);
            diskann::build_disk_index<float>(data_file_path, index_prefix_path,
                                             params.c_str(),
                                             diskann::Metric::L2);
          },
          py::arg("data_file_path"), py::arg("index_prefix_path"), py::arg("R"),
          py::arg("L"), py::arg("final_index_ram_limit"),
          py::arg("indexing_ram_limit"), py::arg("num_threads"))
      .def(
          "search_numpy_input",
          [](DiskANNIndex<float> &self,
             py::array_t<float, py::array::c_style | py::array::forcecast>
                 &      query,
             const _u64 dim, const _u64 knn, const _u64 l_search,
             const _u64 beam_width) {
            py::array_t<unsigned> ids(knn);
            py::array_t<float>    dists(knn);

            std::vector<unsigned> u32_ids(knn);
            std::vector<_u64>     u64_ids(knn);
            QueryStats            stats;

            self.pq_flash_index->cached_beam_search(
                query.data(), knn, l_search, u64_ids.data(),
                dists.mutable_data(), beam_width, &stats);

            auto r = ids.mutable_unchecked<1>();
            for (_u64 i = 0; i < knn; ++i)
              r(i) = (unsigned) u64_ids[i];

            return std::make_pair(ids, dists);
          },
          py::arg("query"), py::arg("dim"), py::arg("knn"), py::arg("l_search"),
          py::arg("beam_width"))
      .def(
          "batch_search_numpy_input",
          [](DiskANNIndex<float> &self,
             py::array_t<float, py::array::c_style | py::array::forcecast>
                 &      queries,
             const _u64 dim, const _u64 num_queries, const _u64 knn,
             const _u64 l_search, const _u64 beam_width,
             const int num_threads) {
            py::array_t<unsigned> ids(knn * num_queries);
            py::array_t<float>    dists(knn * num_queries);

            std::vector<_u64>     u64_ids(knn * num_queries);

#pragma omp parallel for schedule(dynamic, 1)
            for (_u64 i = 0; i < num_queries; i++) {
              self.pq_flash_index->cached_beam_search(
                  queries.data(i), knn, l_search,
                  u64_ids.data() + i * knn, dists.mutable_data(i * knn),
                  beam_width);
            }

            auto r = ids.mutable_unchecked<1>();
            for (_u64 i = 0; i < knn * num_queries; ++i)
              r(i) = (unsigned) u64_ids[i];	    

            return std::make_pair(ids, dists);
          },
          py::arg("queries"), py::arg("dim"), py::arg("num_queries"),
          py::arg("knn"), py::arg("l_search"), py::arg("beam_width"),
          py::arg("num_threads"));
}
