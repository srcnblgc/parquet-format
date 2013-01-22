redfile [![Build Status](https://travis-ci.org/twitter/redfile.png?branch=master)](redfile)
======
Redfile is a columnar storage format that supports nested data.  

## Glossary
  - Block (hdfs block): This means a block in hdfs and the meaning is 
    unchanged for describing this file format.  The file format is 
    designed to work well ontop of hdfs.

  - File: A hdfs file that must include the metadata for the file.
    It does not need to actually contain the data.

  - Row group: A logical horizontal partitioning of the data into rows.
    There is no physical structure that is guaranteed for a row group.
    A row group consists of a column chunk for each column in the dataset.

  - Column chunk: A chunk of the data for a particular column.  These live
    in a particular row group and is guaranteed to be contiguous in the file.

  - Page: Column chunks are divided up into pages.  A page is conceptually
    an indivisible unit (in terms of compression and encoding).  There can
    be multiple page types which is interleaved in a column chunk.

Hierarchically, a file consists of one or more rows groups.  A row group
contains exactly one column chunk per column.  Column chunks contain one or
more pages. 

## Unit of parallelization
  - MapReduce - File/Row Group
  - IO - Column chunk
  - Encoding/Compression - Page

## File format
This file and the thrift definition should be read together to understand the format.

    4-byte magic number "RED1"
    <Column 1 Chunk 1 + Column Metadata>
    <Column 2 Chunk 1 + Column Metadata>
    ...
    <Column N Chunk 1 + Column Metadata>
    <Column 1 Chunk 2 + Column Metadata>
    <Column 2 Chunk 2 + Column Metadata>
    ...
    <Column N Chunk 2 + Column Metadata>
    ...
    <Column 1 Chunk M + Column Metadata>
    <Column 2 Chunk M + Column Metadata>
    ...
    <Column N Chunk M + Column Metadata>
    File Metadata
    4-byte offset from end of file to start of file metadata
    4-byte magic number "RED1"

In the above example, there are N columns in this table, split into M row 
groups.  The file metadata contains the locations of all the column metadata 
start locations.  More details on what is contained in the metdata can be found 
in the thrift files.

Metadata is written after the data to allow for single pass writing.

Readers are expected to first read the file metadata to find all the column 
chunks they are interested in.  The columns chunks should then be read sequentially.

## Metadata
There are three types of metadata: file metadata, column (chunk) metadata and page
header metadata.  These are all serialized using thrift.
    TODO: Agree on thrift serialization.  Compact vs binary?

## Nested Encoding
To encode nested columns, redfile uses the dremel encoding with definition and 
repetition levels.  Definition levels specify how many optional fields in the 
path for the column are defined.  Repetition levels specify at what repeated field
in the path has the value repeated.  The max definition and repetition levels can
be computed from the schema (i.e. how much nesting is there).  This defines the
maximum number of bits required to store the levels (levels are defined for all
values in the column).  The encoding packs the bits as tightly as possible, 
rounding up to the nearest byte.  For example, if the max repetition level was 3
(2 bits) and the max definition level as 4 (3 bits), to encode 30 values, we would
have 30 * 3 = 120 bits = 15 bytes for the definition values followed by 30 * 2 = 60
bits = 8 bytes for the repetition levels.  These level bytes are encoded back to back,
right after the data page header.  The values data is stored immediately after. 

## Nulls
Nullity is encoded in the definition levels.  For example, in a non-nested schema, 
there is 1 bit to encode the nullity for each value in a data page.  A value of 1 
indicates a NULL.  For NULLs, the NULL itself is not encoded in the data following 
the definition and repetition bits.  E.g. a flat schema with a data page consisting 
of only 1000 NULLs, would have 1000 NULL bits (125 bytes) set and nothing else. 

## Column chunks
Column chunks are composed of pages written back to back.  The pages share a common 
header and readers can skip over page they are not interested in.  The data for the 
page follows the header and can be compressed and/or encoded.  The compression and 
encoding is specified in the page metadata.

## Checksumming
Data pages can be individually checksummed.  This allows disabling of checksums at the 
HDFS file level, to better support single row lookups.

## Error recovery
If the file metadata is corrupt, the file is lost.  If the column metdata is corrupt, 
that column chunk is lost (but column chunks for this column in order row groups are 
okay).  If a page header is corrupt, the remaining pages in that chunk are lost.  If 
the data within a page is corrupt, that page is lost.  The file will be more 
resilient to corruption with smaller row groups.

Potential extension: With smaller row groups, the biggest issue is lowing the file 
metadata at the end.  If this happens in the write path, all the data written will 
be unreadable.  This can be fixed by writing the file metadata every Nth row group.  
Each file metadata would be cumulative and include all the row groups written so 
far.  Combining this with the strategy used for rc or avro files using sync markers, 
a reader could recovery partially written files.  

## Configurations
- Row group size: Larger row groups allow for larger column chunks which makes it 
possible to do larger sequential IO.  Larger groups also require more buffering in 
the write path (or a two pass write).  We recommend large row groups (512GB - 1GB).  
Since an entire row group might need to be read, we want it to completely fit on 
one HDFS block.  Therefore, HDFS block sizes should also be set to be larger.  An 
optimized read setup would be: 1GB row groups, 1GB HDFS block size, 1 HDFS block 
per HDFS file.
- Data page size: Data pages should be considered indivisible so smaller data pages 
allow for more fine grained reading (e.g. single row lookup).  Larger page sizes 
incur less space overhead (less page headers) and potentially less parsing overhead 
(processing headers).  Note: for sequential scans, it is not expected to read a page 
at a time; this is not the IO chunk.  We recommend 8KB for page sizes.

## Extensibility
There are many places in the format for compatible extensions:
- File Version: The file metadata contains a version.
- Encodings: Encodings are specified by enum and more can be added in the future.  
- Page types: Additional page types can be added and safely skipped.

## License
Copyright 2013 Twitter, Cloudera and other contributors.

Licensed under the Apache License, Version 2.0: http://www.apache.org/licenses/LICENSE-2.0
