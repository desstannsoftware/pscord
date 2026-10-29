using System.Security.Cryptography;
using LibProsperoPkg.PKG;
using LibProsperoPkg.Util;

if (args.Length < 2)
{
    Console.Error.WriteLine("usage: host-extract <pkg> <outdir> [passcode]");
    return 2;
}

var pkg = args[0];
var outDir = args[1];
var pass = args.Length > 2 ? args[2] : new string('0', 32);
void Log(string m) => Console.WriteLine(m);

var info = ProsperoPackageExtractor.Inspect(pkg);
Console.WriteLine(
    $"type={info.PackageType} retail={info.IsRetail} cid={info.ContentId} enc={info.OuterEncrypted} needKey={info.RequiresSuppliedKey}");
if (info.RequiresSuppliedKey)
{
    Console.Error.WriteLine("retail keyed package needs supplied EKPFS");
    return 1;
}

Directory.CreateDirectory(outDir);

var contentId = info.ContentId
    ?? throw new InvalidOperationException("package has no readable content id");

// Prefer SHA-256 EKPFS; some hosts lack SHA3-256. Fall back to SHA3 when available.
var candidates = new List<byte[]> { Crypto.ComputeKeys(contentId, pass, 1, useSha3: false) };
if (SHA3_256.IsSupported)
    candidates.Add(Crypto.ComputeKeys(contentId, pass, 1, useSha3: true));

Exception? last = null;
foreach (var ekpfs in candidates)
{
    try
    {
        var manifest = ProsperoPackageExtractor.Extract(
            pkg, outDir, ProsperoExtractionKey.FromEkpfs(ekpfs), null, Log);
        Console.WriteLine($"files={manifest.ExtractedFileCount} out={manifest.OutputDirectory}");
        return 0;
    }
    catch (Exception ex)
    {
        last = ex;
        Log($"candidate failed: {ex.Message}");
        // Wipe partial output before the next candidate.
        if (Directory.Exists(outDir))
        {
            foreach (var entry in Directory.EnumerateFileSystemEntries(outDir))
            {
                if (Directory.Exists(entry))
                    Directory.Delete(entry, true);
                else
                    File.Delete(entry);
            }
        }
    }
}

Console.Error.WriteLine(last?.ToString() ?? "extract failed");
return 1;
