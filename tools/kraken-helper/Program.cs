using System.Buffers.Binary;
using Oodle;

// Protocol (stdin/stdout binary):
 // request:  i32 srcLen, i32 flags, i32 firstChunk, i32 dstLen, src[srcLen]
 // response: i32 status, dst[dstLen] on success (status==0)
using var input = Console.OpenStandardInput();
using var output = Console.OpenStandardOutput();
var hdr = new byte[16];
while (true) {
  int n = 0;
  while (n < 16) {
    int r = input.Read(hdr, n, 16 - n);
    if (r <= 0) return;
    n += r;
  }
  int srcLen = BinaryPrimitives.ReadInt32LittleEndian(hdr.AsSpan(0));
  int flags = BinaryPrimitives.ReadInt32LittleEndian(hdr.AsSpan(4));
  int first = BinaryPrimitives.ReadInt32LittleEndian(hdr.AsSpan(8));
  int dstLen = BinaryPrimitives.ReadInt32LittleEndian(hdr.AsSpan(12));
  if (srcLen < 0 || srcLen > 8_000_000 || dstLen < 0 || dstLen > 8_000_000) return;
  var src = new byte[srcLen];
  n = 0;
  while (n < srcLen) {
    int r = input.Read(src, n, srcLen - n);
    if (r <= 0) return;
    n += r;
  }
  var dst = new byte[dstLen];
  int st;
  try { st = (int)KrakenDecoder.DecodeBlock(src, flags, first, dst); }
  catch { st = 99; }
  var outHdr = new byte[4];
  BinaryPrimitives.WriteInt32LittleEndian(outHdr, st);
  output.Write(outHdr);
  if (st == 0) output.Write(dst);
  output.Flush();
}
