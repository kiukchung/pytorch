#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <fstream>

#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"

#ifndef CAFFE2_USE_LITE_PROTO
#include "google/protobuf/text_format.h"
#endif  // !CAFFE2_USE_LITE_PROTO

#include "caffe2/utils/proto_utils.h"
#include "caffe2/core/logging.h"

using ::google::protobuf::Message;
using ::google::protobuf::MessageLite;

namespace caffe2 {

// IO-specific functions: we will deal with the protocol buffer lite and full
// versions differently.

#ifdef CAFFE2_USE_LITE_PROTO

// Lite runtime.

namespace {
class IfstreamInputStream : public ::google::protobuf::io::CopyingInputStream {
 public:
  explicit IfstreamInputStream(const string& filename)
      : ifs_(filename.c_str(), std::ios::in | std::ios::binary) {}
  ~IfstreamInputStream() { ifs_.close(); }

  int Read(void* buffer, int size) {
    if (!ifs_) {
      return -1;
    }
    ifs_.read(static_cast<char*>(buffer), size);
    return ifs_.gcount();
  }

 private:
  std::ifstream ifs_;
};
}  // namespace

bool ReadProtoFromBinaryFile(const char* filename, MessageLite* proto) {
  ::google::protobuf::io::CopyingInputStreamAdaptor stream(
      new IfstreamInputStream(filename));
  stream.SetOwnsCopyingStream(true);
  // Total bytes hard limit / warning limit are set to 1GB and 512MB
  // respectively.
  ::google::protobuf::io::CodedInputStream coded_stream(&stream);
  coded_stream.SetTotalBytesLimit(1024LL << 20, 512LL << 20);
  return proto->ParseFromCodedStream(&coded_stream);
}

void WriteProtoToBinaryFile(const MessageLite& proto, const char* filename) {
  CAFFE_LOG_FATAL << "Not implemented yet.";
}

#else  // CAFFE2_USE_LITE_PROTO

// Full protocol buffer.

using ::google::protobuf::io::FileInputStream;
using ::google::protobuf::io::FileOutputStream;
using ::google::protobuf::io::ZeroCopyInputStream;
using ::google::protobuf::io::CodedInputStream;
using ::google::protobuf::io::ZeroCopyOutputStream;
using ::google::protobuf::io::CodedOutputStream;

bool ReadProtoFromTextFile(const char* filename, Message* proto) {
  int fd = open(filename, O_RDONLY);
  CAFFE_CHECK_NE(fd, -1) << "File not found: " << filename;
  FileInputStream* input = new FileInputStream(fd);
  bool success = google::protobuf::TextFormat::Parse(input, proto);
  delete input;
  close(fd);
  return success;
}

void WriteProtoToTextFile(const Message& proto, const char* filename) {
  int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  FileOutputStream* output = new FileOutputStream(fd);
  CAFFE_CHECK(google::protobuf::TextFormat::Print(proto, output));
  delete output;
  close(fd);
}

bool ReadProtoFromBinaryFile(const char* filename, MessageLite* proto) {
  int fd = open(filename, O_RDONLY);
  CAFFE_CHECK_NE(fd, -1) << "File not found: " << filename;
  std::unique_ptr<ZeroCopyInputStream> raw_input(new FileInputStream(fd));
  std::unique_ptr<CodedInputStream> coded_input(
      new CodedInputStream(raw_input.get()));
  // A hack to manually allow using very large protocol buffers.
  coded_input->SetTotalBytesLimit(1073741824, 536870912);
  bool success = proto->ParseFromCodedStream(coded_input.get());
  coded_input.reset();
  raw_input.reset();
  close(fd);
  return success;
}

void WriteProtoToBinaryFile(const MessageLite& proto, const char* filename) {
  int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  CAFFE_CHECK_NE(fd, -1) << "File cannot be created: " << filename
                   << " error number: " << errno;
  std::unique_ptr<ZeroCopyOutputStream> raw_output(new FileOutputStream(fd));
  std::unique_ptr<CodedOutputStream> coded_output(
      new CodedOutputStream(raw_output.get()));
  CAFFE_CHECK(proto.SerializeToCodedStream(coded_output.get()));
  coded_output.reset();
  raw_output.reset();
  close(fd);
}

#endif  // CAFFE2_USE_LITE_PROTO


#define CAFFE2_MAKE_SINGULAR_ARGUMENT(T, fieldname)                            \
template <>                                                                    \
Argument MakeArgument(const string& name, const T& value) {                    \
  Argument arg;                                                                \
  arg.set_name(name);                                                          \
  arg.set_##fieldname(value);                                                  \
  return arg;                                                                  \
}

CAFFE2_MAKE_SINGULAR_ARGUMENT(float, f)
CAFFE2_MAKE_SINGULAR_ARGUMENT(int, i)
CAFFE2_MAKE_SINGULAR_ARGUMENT(string, s)
#undef CAFFE2_MAKE_SINGULAR_ARGUMENT

template <>
Argument MakeArgument(const string& name, const MessageLite& value) {
  Argument arg;
  arg.set_name(name);
  arg.set_s(value.SerializeAsString());
  return arg;
}


#define CAFFE2_MAKE_REPEATED_ARGUMENT(T, fieldname)                            \
template <>                                                                    \
Argument MakeArgument(const string& name, const vector<T>& value) {            \
  Argument arg;                                                                \
  arg.set_name(name);                                                          \
  for (const auto& v : value) {                                                \
    arg.add_##fieldname(v);                                                    \
  }                                                                            \
}

CAFFE2_MAKE_REPEATED_ARGUMENT(float, floats)
CAFFE2_MAKE_REPEATED_ARGUMENT(int, ints)
CAFFE2_MAKE_REPEATED_ARGUMENT(string, strings)
#undef CAFFE2_MAKE_REPEATED_ARGUMENT

const Argument& GetArgument(const OperatorDef& def, const string& name) {
  for (const Argument& arg : def.arg()) {
    if (arg.name() == name) {
      return arg;
    }
  }
  CAFFE_LOG_FATAL << "Argument named " << name << " does not exist.";
  // To suppress compiler warning of return values. This will never execute.
  static Argument _dummy_arg_to_suppress_compiler_warning;
  return _dummy_arg_to_suppress_compiler_warning;
}

Argument* GetMutableArgument(
    const string& name, const bool create_if_missing, OperatorDef* def) {
  for (int i = 0; i < def->arg_size(); ++i) {
    if (def->arg(i).name() == name) {
      return def->mutable_arg(i);
    }
  }
  // If no argument of the right name is found...
  if (create_if_missing) {
    Argument* arg = def->add_arg();
    arg->set_name(name);
    return arg;
  } else {
    return nullptr;
  }
}

}  // namespace caffe2
