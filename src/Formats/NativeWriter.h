#pragma once

#include <Core/Block.h>
#include <DataTypes/IDataType.h>
#include <Formats/FormatSettings.h>

namespace DB
{

class WriteBuffer;
class CompressedWriteBuffer;
struct IndexForNativeFormat;

/** Serializes the stream of blocks in their native binary format (with names and column types).
  * Designed for communication between servers.
  *
  * A stream can be specified to write the index. The index contains offsets to each part of each column.
  * If an `append` is made to an existing file, and you need to write the index, then specify `initial_size_of_file`.
  */
class NativeWriter
{
public:
    /** If non-zero client_revision is specified, additional block information can be written.
      */
    NativeWriter(
        WriteBuffer & ostr_, UInt64 client_revision_, const Block & header_, std::optional<FormatSettings> format_settings_ = std::nullopt, bool remove_low_cardinality_ = false,
        IndexForNativeFormat * index_ = nullptr, size_t initial_size_of_file_ = 0);

    Block getHeader() const { return header; }

    /// Returns the number of bytes written.
    size_t write(const Block & block);
    void flush();

    static SerializationPtr getSerialization(UInt64 client_revision, const ColumnWithTypeAndName & column);

    static void writeData(const ISerialization & serialization, const ColumnPtr & column, WriteBuffer & ostr, const std::optional<FormatSettings> & format_settings, UInt64 offset, UInt64 limit, UInt64 client_revision);

private:
    WriteBuffer & ostr;
    UInt64 client_revision;
    Block header;
    IndexForNativeFormat * index = nullptr;
    size_t initial_size_of_file;    /// The initial size of the data file, if `append` done. Used for the index.
    /// If you need to write index, then `ostr` must be a CompressedWriteBuffer.
    CompressedWriteBuffer * ostr_concrete = nullptr;

    bool remove_low_cardinality;
    std::optional<FormatSettings> format_settings;
};

}
