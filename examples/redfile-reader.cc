// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iostream>
#include <sstream>
#include <vector>
#include <gflags/gflags.h>
#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/protocol/TDebugProtocol.h>
#include <thrift/TApplicationException.h>
#include <thrift/transport/TBufferTransports.h>
#include "gen-cpp/redfile_types.h"

// TCompactProtocol requires some #defines to work right.  
// TODO: is there a better include to use?
#define SIGNED_RIGHT_SHIFT_IS 1
#define ARITHMETIC_RIGHT_SHIFT 1
#include <thrift/protocol/TCompactProtocol.h>

DEFINE_string(file, "", "File to read");
DEFINE_int32(values_per_data_page, -1, "Number of values to output per datapage.");
DEFINE_bool(output_page_header, false, "If true, output page headers to stderr.");
DEFINE_bool(output_to_csv, false, "If true, output csv to stdout.  This can be very slow");

using namespace boost;
using namespace redfile;
using namespace std;
using namespace apache::thrift;
using namespace apache::thrift::protocol;
using namespace apache::thrift::transport;

// Some code is replicated to make this more stand-alone.
const uint8_t REDFILE_VERSION_NUMBER[] = {'R', 'E', 'D', '1'};

int base_row_idx;
vector<vector<string> > rows_csv;

shared_ptr<TProtocol> CreateDeserializeProtocol(
    shared_ptr<TMemoryBuffer> mem, bool compact) {
  if (compact) {
    TCompactProtocolFactoryT<TMemoryBuffer> tproto_factory;
    return tproto_factory.getProtocol(mem);
  } else {
    TBinaryProtocolFactoryT<TMemoryBuffer> tproto_factory;
    return tproto_factory.getProtocol(mem);
  }
}

// Deserialize a thrift message from buf/len.  buf/len must at least contain
// all the bytes needed to store the thrift message.  On return, len will be
// set to the actual length of the header.
template <class T>
bool DeserializeThriftMsg(uint8_t* buf, uint32_t* len, bool compact, 
    T* deserialized_msg) {
  // Deserialize msg bytes into c++ thrift msg using memory transport.
  shared_ptr<TMemoryBuffer> tmem_transport(new TMemoryBuffer(buf, *len));
  shared_ptr<TProtocol> tproto = CreateDeserializeProtocol(tmem_transport, compact);
  try {
    deserialized_msg->read(tproto.get());
  } catch (apache::thrift::protocol::TProtocolException& e) {
    cerr << "couldn't deserialize thrift msg:\n" << e.what();
    return false;
  }
  uint32_t bytes_left = tmem_transport->available_read();
  *len = *len - bytes_left;
  return true;
}

template<typename T>
void OutputDataPage(uint8_t* definition_data, uint8_t* data, int num_values) {
  int data_index = 0;
  T* d = reinterpret_cast<T*>(data);
  for (int n = 0; n < num_values; ++n) {
    int def_byte = n / 8;
    int def_bit = n % 8;
    if ((definition_data[def_byte] & (1 << def_bit)) != 0) {
      if (FLAGS_output_to_csv) {
        rows_csv[base_row_idx + n].push_back("");
      } else {
        cerr << "Value: NULL" << endl;
      }
    } else {
      if (FLAGS_output_to_csv) {
        stringstream ss;
        ss << d[data_index];
        rows_csv[base_row_idx + n].push_back(ss.str());
      } else {
        cerr << "Value: " << d[data_index] << endl;
      }
      ++data_index;
    }
  }
}

template<>
void OutputDataPage<bool>(uint8_t* definition_data, uint8_t* data, int num_values) {
  int data_index = 0;
  for (int n = 0; n < num_values; ++n) {
    int def_byte = n / 8;
    int def_bit = n % 8;
    if ((definition_data[def_byte] & (1 << def_bit)) != 0) {
      if (FLAGS_output_to_csv) {
        rows_csv[base_row_idx + n].push_back("");
      } else {
        cerr << "Value: NULL" << endl;
      }
    } else {
      int val_byte = data_index / 8;
      int val_bit = data_index % 8;
      ++data_index;
      bool val = (data[val_byte] & (1 << val_bit)) != 0;
      if (FLAGS_output_to_csv) {
        rows_csv[base_row_idx + n].push_back(val ? "true" : "false");
      } else {
        cerr << "Value: " << (val ? "true" : "false") << endl;
      }
    }
  }
}

template<>
void OutputDataPage<string>(uint8_t* definition_data, uint8_t* data, int num_values) {
  for (int n = 0; n < num_values; ++n) {
    int def_byte = n / 8;
    int def_bit = n % 8;
    if ((definition_data[def_byte] & (1 << def_bit)) != 0) {
      cerr << "Value: NULL" << endl;
      if (FLAGS_output_to_csv) rows_csv[base_row_idx + n].push_back("");
    } else {
      int32_t len = *reinterpret_cast<int32_t*>(data);
      data += sizeof(int32_t);
      if (FLAGS_output_to_csv) {
        rows_csv[base_row_idx + n].push_back(string(reinterpret_cast<char*>(data), len));
      } else {
        cerr << "Value: " << string(reinterpret_cast<char*>(data), len) << endl;
      }
      data += len;
    }
  }
}

// Simple utility to read a redfile on local disk.  This utility validates the
// file is correctly formed and can output values from each data page.  The
// entire file is buffered in memory so this is not suitable for large files.
int main(int argc, char** argv) {
  google::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_file.size() == 0) {
    cout << "Must specify input file." << endl;
    return -1;
  }

  if (FLAGS_output_to_csv) {
    FLAGS_values_per_data_page = -1;
  }

  FILE* file = fopen(FLAGS_file.c_str(), "r");
  assert(file != NULL);

  fseek(file, 0L, SEEK_END);
  size_t file_len = ftell(file);
  fseek(file, 0L, SEEK_SET);

  cerr << "File Length: " << file_len << endl;

  uint8_t* buffer = reinterpret_cast<uint8_t*>(malloc(file_len));
  size_t bytes_read = fread(buffer, 1, file_len, file);
  assert(bytes_read == file_len);

  // Check file starts and ends with magic bytes
  assert(
      memcmp(buffer, REDFILE_VERSION_NUMBER, sizeof(REDFILE_VERSION_NUMBER)) == 0);
  assert(memcmp(buffer + file_len - sizeof(REDFILE_VERSION_NUMBER), 
      REDFILE_VERSION_NUMBER, sizeof(REDFILE_VERSION_NUMBER)) == 0);

  // Get metadata
  uint8_t* metadata_offset_ptr = 
      buffer + file_len - sizeof(REDFILE_VERSION_NUMBER) - sizeof(uint32_t);
  uint32_t metadata_offset = *reinterpret_cast<uint32_t*>(metadata_offset_ptr);
  cerr << "Metadata offset: " << metadata_offset << endl;

  uint8_t* metadata = buffer + file_len - metadata_offset;
  uint32_t metadata_len = metadata_offset - sizeof(REDFILE_VERSION_NUMBER) - sizeof(uint32_t);

  FileMetaData file_metadata;
  bool status = DeserializeThriftMsg(metadata, &metadata_len, true, &file_metadata);
  assert(status);
  cerr << ThriftDebugString(file_metadata) << endl;

  int pages_skipped = 0;
  int pages_read = 0;
  int num_rows = 0;
  int total_page_header_size = 0;
  int total_column_data_size = 0;
  vector<int> column_sizes;

  for (int i = 0; i < file_metadata.row_groups.size(); ++i) {
    cerr << "Reading row group " << i << endl;
    RowGroup& rg = file_metadata.row_groups[i];
    column_sizes.resize(rg.columns.size());

    int rg_base_index = rows_csv.size();

    for (int c = 0; c < rg.columns.size(); ++c) {
      base_row_idx = rg_base_index;
      cerr << "  Reading column " << c << endl;
      ColumnChunk& col = rg.columns[c];
      
      uint8_t* col_end = buffer + col.file_offset;
      uint8_t* data = buffer + col.meta_data.data_page_offset;
      
      // Loop through the entire column chunk.  This lets us walk all the pages.
      while (data < col_end) {
        uint32_t header_size = file_len - col.file_offset;
        PageHeader header;
        status = DeserializeThriftMsg(data, &header_size, true, &header);
        assert(status);
        if (FLAGS_output_page_header) {
          cerr << ThriftDebugString(header) << endl;
        }
        data += header_size;
        total_page_header_size += header_size;
        column_sizes[c] += header.compressed_page_size;
        total_column_data_size += header.compressed_page_size;
          
        // Skip non-data or non-plain encoding
        if (header.type != PageType::DATA_PAGE || 
            header.data_page.encoding != Encoding::PLAIN) {
          ++pages_skipped;
          data += header.compressed_page_size;
          continue;
        }
        ++pages_read;

        int num_values = header.data_page.num_values;
        if (c == 0) num_rows += num_values;

        uint8_t* definition_data = data;
        uint8_t* values = data + (num_values / 8) + (num_values % 8 != 0);
    
        int num_output_values = num_values;
        if (FLAGS_values_per_data_page >= 0) {
          num_output_values = min(num_values, FLAGS_values_per_data_page);
        }
        if (c == 0 && FLAGS_output_to_csv) {
          rows_csv.resize(rows_csv.size() + num_output_values);
        }

        switch (col.meta_data.type) {
          case Type::BOOLEAN:
            OutputDataPage<bool>(definition_data, values, num_output_values);
            break;
          case Type::INT32: 
            OutputDataPage<int32_t>(definition_data, values, num_output_values);
            break;
          case Type::INT64: 
            OutputDataPage<int64_t>(definition_data, values, num_output_values);
            break;
          case Type::FLOAT:
            OutputDataPage<float>(definition_data, values, num_output_values);
            break;
          case Type::DOUBLE:
            OutputDataPage<double>(definition_data, values, num_output_values);
            break;
          case Type::BYTE_ARRAY:
            OutputDataPage<string>(definition_data, values, num_output_values);
            break;
          default:
            // TODO: INT96
            assert(0);
        }
        data += header.compressed_page_size;
        base_row_idx += num_output_values;
      }
      // Check that we ended exactly where we should have
      assert(data == col_end);
    }
  }
  stringstream ss;
  ss << "\nSummary:\n"
     << "  Rows: " << num_rows << endl
     << "  Read pages: " << pages_read << endl
     << "  Skipped pages: " << pages_skipped << endl
     << "  Metadata size: " << metadata_len 
     << "(" << (metadata_len / (double)file_len) << ")" << endl
     << "  Total page header size: " << total_page_header_size 
     << "(" << (total_page_header_size / (double)file_len) << ")" << endl;
  ss << "  Column byte sizes: " << total_column_data_size 
     << "(" << (total_column_data_size / (double)file_len) << ")" << endl;
  for (int i = 0; i < column_sizes.size(); ++i) {
    ss << "    " << "Col " << i << ": " << column_sizes[i]
       << "(" << (column_sizes[i] / (double)file_len) << ")" << endl;
  }
  cerr << ss.str() << endl;

  // Join all rows and output to csv
  for (int i = 0; i < rows_csv.size(); ++i) {
    stringstream ss;
    for (int j = 0; j < rows_csv[i].size(); ++j) {
      ss << rows_csv[i][j];
      if (j != rows_csv[i].size() - 1) {
        ss << "|";
      }
    }
    cout << ss.str() << endl;
  }

  return 0;
}
