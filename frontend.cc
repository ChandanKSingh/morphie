// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
// License for the specific language governing permissions and limitations under
// the License.

// This file contains functions for initializing and running the different
// analyzers in logle. It also contains utility functions for the file I/O
// required to obtain the input for an analyzer or save the output generated by
// an analyzer.
#include "frontend.h"

#include <fstream>
#include <memory>
#include <type_traits>
#include <utility>

#include "analyzers/examples/account_access_analyzer.h"
#include "analyzers/examples/curio_analyzer.h"
#include "analyzers/plaso/plaso_analyzer.h"
#include "base/string.h"
#include "json/json.h"
#include "util/csv.h"
#include "util/json_reader.h"
#include "util/logging.h"
#include "util/status.h"
#include "util/string_utils.h"

namespace {

namespace util = morphie::util;

// Error messages.
const char kInvalidAnalyzerErr[] =
    "Invalid analysis. The analysis must be one of 'curio', 'mail', or "
    "'plaso'.";
const char kOpenFileErr[] = "Error opening file: ";
const char kInvalidPlasoOption[] =
    "Unsupported input parameter. Plaso analyzer supports only json_file and "
    "json_stream_file.";

// Returns a pair consisting of a status object and a CSV parser for 'filename'.
// The return value is:
//  - OK if 'filename' could be opened successfully. In this case, the second
//    element of the returned pair is a CSV parser and the input 'file_ptr'
//    points to the open file.
//  - An error code if the file could not be opened. No parser is returned and
//    'file_ptr' should not be used to read from the file.
//
// The file pointer is taken as a separate input to ensure it has a longer
// lifetime than the parser so that the file can be closed after parsing is
// complete.
std::pair<util::Status, std::unique_ptr<util::CSVParser>> GetCSVParser(
    const std::string& filename) {
  std::ifstream* csv_stream = new std::ifstream(filename);
  if (csv_stream == nullptr || !*csv_stream) {
    return {util::Status(morphie::Code::EXTERNAL,
                         util::StrCat(kOpenFileErr, filename)), nullptr};
  }
  // The CSV parser takes ownership of the csv_stream and will close the file
  // once parsing is done.
  std::unique_ptr<util::CSVParser> parser(new util::CSVParser(csv_stream));
  return {util::Status::OK, std::move(parser)};
}

// Returns a JSON document extracted from 'filename'. Comments in the JSON
// document will be ignored.
std::unique_ptr<Json::Value> GetJsonDoc(const std::string& filename) {
  Json::Reader json_reader;
  std::unique_ptr<Json::Value> json_doc(new Json::Value);
  std::ifstream json_stream(filename);
  json_reader.parse(json_stream, *json_doc, false /*Do not parse comments*/);
  return json_doc;
}

// Writes the string 'contents' to 'filename'. Returns
//  - OK if 'filename' could be opened for writing, written to, and closed
//    successfully.
//  - an error code with explanation otherwise.
util::Status WriteToFile(const std::string& filename,
                         const std::string& contents) {
  std::ofstream out_file;
  out_file.open(filename, std::ofstream::out);
  // An ofstream automatically closes a file when it goes out of scope, so the
  // early returns will not leave the file open. The file is nonetheless
  // explicitly closed only to be able to detect errors.
  if (!out_file) {
    return util::Status(morphie::Code::EXTERNAL,
                       util::StrCat("Error opening file: ", filename));
  }
  out_file << contents;

  if (!out_file) {
    return util::Status(morphie::Code::INTERNAL,
                       util::StrCat("Error writing to file: ", filename));
  }
  out_file.close();
  if (!out_file) {
    return util::Status(morphie::Code::EXTERNAL,
                       util::StrCat("Error closing file: ", filename));
  }
  return util::Status::OK;
}

}  // namespace

namespace morphie {
namespace frontend {

// Runs the Curio analyzer in curio_analyzer.h on the input. Returns an error
// code if the input is not in JSON format.
util::Status RunCurioAnalyzer(const AnalysisOptions& options,
                              string* output_graph) {
  if (!options.has_json_file()) {
    return util::Status(morphie::Code::INVALID_ARGUMENT,
                        "The Curio analyzer requires a JSON input file.");
  }
  std::unique_ptr<Json::Value> json_doc = GetJsonDoc(options.json_file());
  CurioAnalyzer curio_analyzer;
  util::Status status = curio_analyzer.Initialize(std::move(json_doc));
  if (!status.ok()) {
    return status;
  }
  status = curio_analyzer.BuildDependencyGraph();
  if (!status.ok()) {
    return status;
  }
  *output_graph = curio_analyzer.DependencyGraphAsDot();
  return status;
}

// Runs the Plaso analyzer in plaso_analyzer.h on the input. The input can be in
// JSON or JSON stream format. Returns an error code if file I/O fails. If the
// analyzer is run successfully, a GraphViz DOT representation of the
// constructed graph is returned in 'output_graph'.
util::Status RunPlasoAnalyzer(const AnalysisOptions& options,
                              string* output_graph) {
  util::Status status;

  bool show_all_sources = options.has_plaso_options()
                              ? options.plaso_options().show_all_sources()
                              : false;
  PlasoAnalyzer plaso_analyzer(show_all_sources);
  std::ifstream* input_stream = nullptr;
  switch (options.input_file_case()) {
    case AnalysisOptions::InputFileCase::kJsonFile:{
      input_stream = new std::ifstream(options.json_file());
      if (!input_stream->is_open()){
        return util::Status(morphie::Code::EXTERNAL,
                            util::StrCat(kOpenFileErr, options.json_file()));
      }
      status = plaso_analyzer.Initialize(new morphie::FullJson(input_stream));
      break;
    }
    case AnalysisOptions::InputFileCase::kJsonStreamFile:{
      input_stream = new std::ifstream(options.json_stream_file());
      if (!input_stream->is_open()){
        return util::Status(morphie::Code::EXTERNAL,
                            util::StrCat(kOpenFileErr,
                            options.json_stream_file()));
      }
      status = plaso_analyzer.Initialize(new morphie::StreamJson(input_stream));
      break;
    }
    default:{
      return util::Status(morphie::Code::EXTERNAL, kInvalidPlasoOption);
      break;
    }
  }
  if (!status.ok()) {
    return status;
  }
  plaso_analyzer.BuildPlasoGraph();
  input_stream->close();
  if (options.has_output_dot_file()) {
    *output_graph = plaso_analyzer.PlasoGraphDot();
  } else if (options.has_output_pbtxt_file()) {
    *output_graph = plaso_analyzer.PlasoGraphPbTxt();
  }
  return util::Status::OK;
}

// Runs the analyzer in account_access_analyzer.h on the input. Returns
//  - INVALID_ARGUMENT if the input is not in CSV format or if
//    file I/O causes an error or if graph initialization or construction fails.
//  - OK otherwise.
// If OK is returned, 'output_graph' contains a GraphViz DOT graph.
util::Status RunMailAccessAnalyzer(const AnalysisOptions& options,
                                   string* output_graph) {
  if (!options.has_csv_file()) {
    return util::Status(morphie::Code::INVALID_ARGUMENT,
                        "The access analyzer requires a CSV input file.");
  }
  AccessAnalyzer access_analyzer;
  std::pair<util::Status, std::unique_ptr<util::CSVParser>> result =
      GetCSVParser(options.csv_file());

  util::Status status = result.first;
  if (!status.ok()) {
    return status;
  }
  status = access_analyzer.Initialize(std::move(result.second));
  if (!status.ok()) {
    return status;
  }
  status = access_analyzer.BuildAccessGraph();
  if (!status.ok()) {
    return status;
  }
  *output_graph = access_analyzer.AccessGraphAsDot();
  return util::Status::OK;
}

// Invokes the specified analyzer on an input data source and after analysis,
// writes a graph to a file if required.
util::Status Run(const AnalysisOptions& options) {
  util::Status status = util::Status::OK;
  string output_graph;
  // Invoke an analyzer.
  if (!options.has_analyzer()) {
    return util::Status(Code::INVALID_ARGUMENT, kInvalidAnalyzerErr);
  } else if (options.analyzer() == "curio") {
    status = RunCurioAnalyzer(options, &output_graph);
  } else if (options.analyzer() == "mail") {
    status = RunMailAccessAnalyzer(options, &output_graph);
  } else if (options.analyzer() == "plaso") {
    status = RunPlasoAnalyzer(options, &output_graph);
  } else {
    return util::Status(Code::INVALID_ARGUMENT, kInvalidAnalyzerErr);
  }
  // Write the output of the analysis and return.
  if (!status.ok() || output_graph == "") {
    return status;
  }
  if (options.output_dot_file() != "") {
    status = WriteToFile(options.output_dot_file(), output_graph);
  }
  if (options.output_pbtxt_file() != "") {
    status = WriteToFile(options.output_pbtxt_file(), output_graph);
  }
  return status;
}

}  // namespace frontend
}  // namespace morphie
