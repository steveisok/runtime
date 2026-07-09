// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Reflection.PortableExecutable;
using System.Buffers.Binary;
using System.Text;
using Microsoft.Diagnostics.DataContractReader;
using Microsoft.Diagnostics.DataContractReader.Contracts;
using Microsoft.Diagnostics.Runtime;

namespace Microsoft.DotNet.Diagnostics.CdacDumpInspect;

internal static class DumpHelpers
{
    private static readonly string[] s_coreClrModuleNames = ["coreclr.dll", "libcoreclr.so", "libcoreclr.dylib"];

    public static ulong FindContractDescriptor(DataTarget dt)
    {
        // First pass: look in known CoreCLR modules.
        // Second pass: check all remaining modules (covers NativeAOT where the export is in the app binary).
        ulong fallback = 0;
        foreach (ModuleInfo module in dt.DataReader.EnumerateModules())
        {
            ulong addr = module.GetExportSymbolAddress("DotNetRuntimeContractDescriptor");
            if (addr == 0)
                continue;

            if (dt.DataReader.PointerSize == 4)
                addr &= 0xFFFF_FFFF;

            string? fileName = module.FileName;
            if (fileName is not null)
            {
                int lastSep = Math.Max(fileName.LastIndexOf('/'), fileName.LastIndexOf('\\'));
                string name = lastSep >= 0 ? fileName[(lastSep + 1)..] : fileName;
                if (s_coreClrModuleNames.Contains(name, StringComparer.OrdinalIgnoreCase))
                    return addr;
            }

            if (fallback == 0)
                fallback = addr;
        }

        if (fallback != 0)
            return fallback;

        ulong runtimeBase = TryReadSpecialDiagRuntimeBase(dt);
        if (runtimeBase != 0)
        {
            ulong addr = TryGetMachOExportSymbolAddress(dt, runtimeBase, "DotNetRuntimeContractDescriptor");
            if (addr != 0)
                return addr;
        }

        throw new InvalidOperationException("Could not find DotNetRuntimeContractDescriptor export.");
    }

    private static ulong TryReadSpecialDiagRuntimeBase(DataTarget dt)
    {
        ulong specialDiagInfoAddress = dt.DataReader.PointerSize == 4 ? 0x7fff1000u : 0x7fffffff10000000ul;
        Span<byte> header = stackalloc byte[40];
        if (dt.DataReader.Read(specialDiagInfoAddress, header) != header.Length)
            return 0;

        if (!header.Slice(0, "DIAGINFOHEADER".Length).SequenceEqual(Encoding.ASCII.GetBytes("DIAGINFOHEADER")))
            return 0;

        int version = BinaryPrimitives.ReadInt32LittleEndian(header.Slice(16, 4));
        return version >= 2 ? BinaryPrimitives.ReadUInt64LittleEndian(header.Slice(32, 8)) : 0;
    }

    private static ulong TryGetMachOExportSymbolAddress(DataTarget dt, ulong imageBase, string symbolName)
    {
        Span<byte> header = stackalloc byte[32];
        if (dt.DataReader.Read(imageBase, header) != header.Length)
            return 0;

        const uint MH_MAGIC_64 = 0xfeedfacf;
        if (BinaryPrimitives.ReadUInt32LittleEndian(header) != MH_MAGIC_64)
            return 0;

        uint ncmds = BinaryPrimitives.ReadUInt32LittleEndian(header.Slice(16, 4));
        uint sizeofcmds = BinaryPrimitives.ReadUInt32LittleEndian(header.Slice(20, 4));
        if (ncmds == 0 || sizeofcmds == 0 || sizeofcmds > 1024 * 1024)
            return 0;

        byte[] commands = new byte[sizeofcmds];
        if (dt.DataReader.Read(imageBase + 32, commands) != commands.Length)
            return 0;

        List<SegmentCommand64> segments = [];
        SymtabCommand? symtab = null;
        int offset = 0;
        for (uint i = 0; i < ncmds && offset + 8 <= commands.Length; i++)
        {
            uint cmd = BinaryPrimitives.ReadUInt32LittleEndian(commands.AsSpan(offset, 4));
            uint cmdsize = BinaryPrimitives.ReadUInt32LittleEndian(commands.AsSpan(offset + 4, 4));
            if (cmdsize < 8 || offset + cmdsize > commands.Length)
                return 0;

            const uint LC_SEGMENT_64 = 0x19;
            const uint LC_SYMTAB = 0x2;
            if (cmd == LC_SEGMENT_64 && cmdsize >= 72)
            {
                ReadOnlySpan<byte> c = commands.AsSpan(offset);
                segments.Add(new SegmentCommand64(
                    ReadFixedAscii(c.Slice(8, 16)),
                    BinaryPrimitives.ReadUInt64LittleEndian(c.Slice(24, 8)),
                    BinaryPrimitives.ReadUInt64LittleEndian(c.Slice(32, 8)),
                    BinaryPrimitives.ReadUInt64LittleEndian(c.Slice(40, 8)),
                    BinaryPrimitives.ReadUInt64LittleEndian(c.Slice(48, 8))));
            }
            else if (cmd == LC_SYMTAB && cmdsize >= 24)
            {
                ReadOnlySpan<byte> c = commands.AsSpan(offset);
                symtab = new SymtabCommand(
                    BinaryPrimitives.ReadUInt32LittleEndian(c.Slice(8, 4)),
                    BinaryPrimitives.ReadUInt32LittleEndian(c.Slice(12, 4)),
                    BinaryPrimitives.ReadUInt32LittleEndian(c.Slice(16, 4)),
                    BinaryPrimitives.ReadUInt32LittleEndian(c.Slice(20, 4)));
            }

            offset += (int)cmdsize;
        }

        if (symtab is null || segments.Count == 0 || symtab.Value.NSyms == 0 || symtab.Value.StrSize == 0)
            return 0;

        ulong loadBias = imageBase;
        foreach (SegmentCommand64 segment in segments)
        {
            if (segment.Name == "__TEXT")
            {
                loadBias = imageBase - segment.VmAddr;
                break;
            }
        }

        ulong symbolTableAddress = GetMachOAddressFromFileOffset(imageBase, segments, symtab.Value.SymOff);
        ulong stringTableAddress = GetMachOAddressFromFileOffset(imageBase, segments, symtab.Value.StrOff);
        if (symbolTableAddress == 0 || stringTableAddress == 0)
            return 0;

        const uint N_STAB = 0xe0;
        const uint N_TYPE = 0x0e;
        const uint N_SECT = 0x0e;
        byte[] nlist = new byte[16];
        for (uint i = 0; i < symtab.Value.NSyms; i++)
        {
            if (dt.DataReader.Read(symbolTableAddress + i * 16, nlist) != nlist.Length)
                return 0;

            uint strx = BinaryPrimitives.ReadUInt32LittleEndian(nlist.AsSpan(0, 4));
            byte type = nlist[4];
            if (strx == 0 || strx >= symtab.Value.StrSize || (type & N_STAB) != 0 || (type & N_TYPE) != N_SECT)
                continue;

            string? currentName = ReadNullTerminatedAscii(dt, stringTableAddress + strx, symtab.Value.StrSize - strx);
            if (currentName is null)
                continue;

            if (currentName.StartsWith('_'))
                currentName = currentName[1..];

            if (currentName == symbolName)
                return loadBias + BinaryPrimitives.ReadUInt64LittleEndian(nlist.AsSpan(8, 8));
        }

        return 0;
    }

    private static ulong GetMachOAddressFromFileOffset(ulong imageBase, List<SegmentCommand64> segments, uint fileOffset)
    {
        foreach (SegmentCommand64 segment in segments)
        {
            if (fileOffset >= segment.FileOff && fileOffset < segment.FileOff + segment.FileSize)
                return imageBase + fileOffset + segment.VmAddr - segment.FileOff;
        }

        return imageBase + fileOffset;
    }

    private static string? ReadNullTerminatedAscii(DataTarget dt, ulong address, ulong maxLength)
    {
        int length = (int)Math.Min(maxLength, 4096);
        byte[] buffer = new byte[length];
        int read = dt.DataReader.Read(address, buffer);
        if (read <= 0)
            return null;

        int terminator = Array.IndexOf(buffer, (byte)0, 0, read);
        if (terminator < 0)
            return null;

        return Encoding.ASCII.GetString(buffer, 0, terminator);
    }

    private static string ReadFixedAscii(ReadOnlySpan<byte> buffer)
    {
        int terminator = buffer.IndexOf((byte)0);
        if (terminator < 0)
            terminator = buffer.Length;

        return Encoding.ASCII.GetString(buffer.Slice(0, terminator));
    }

    private readonly record struct SegmentCommand64(string Name, ulong VmAddr, ulong VmSize, ulong FileOff, ulong FileSize);
    private readonly record struct SymtabCommand(uint SymOff, uint NSyms, uint StrOff, uint StrSize);

    public static ContractDescriptorTarget CreateCdacTarget(DataTarget dt)
    {
        ulong contractAddr = FindContractDescriptor(dt);

        // Module map derived lazily from the cDAC Loader contract. Managed PE assemblies are not
        // dyld images, so ClrMD's Mach-O module enumeration does not surface them; the Loader
        // contract does. This is what lets us drop image-backed content (metadata) from the dump
        // and read it from the on-disk assembly instead — matching how real minidumps work.
        LoaderImageMap imageMap = new();

        if (!ContractDescriptorTarget.TryCreate(
                contractAddr,
                (ulong address, Span<byte> buffer) => ReadWithImageFallback(dt, imageMap, address, buffer),
                (ulong address, Span<byte> buffer) => -1,
                (uint threadId, uint contextFlags, Span<byte> buffer) =>
                    dt.DataReader.GetThreadContext(threadId, contextFlags, buffer) ? 0 : -1,
                (uint threadId, ReadOnlySpan<byte> context) => -1,
                (ulong size, out ulong allocatedAddress) => { allocatedAddress = 0; return -1; },
                [CoreCLRContracts.Register],
                out ContractDescriptorTarget? target))
        {
            throw new InvalidOperationException("Failed to create cDAC target.");
        }

        imageMap.Target = target;
        return target!;
    }

    /// <summary>
    /// Lazily-built map of loaded managed module ranges (base, size, simple name) obtained from the
    /// cDAC Loader contract. Used to locate the on-disk assembly that backs an address when the dump
    /// does not contain the requested bytes.
    /// </summary>
    private sealed class LoaderImageMap
    {
        internal readonly record struct Entry(ulong Base, ulong Size, uint Flags, string SimpleName);

        private Entry[]? _entries;
        private bool _building;
        public ContractDescriptorTarget? Target { get; set; }

        public bool TryFind(ulong address, out Entry entry)
        {
            entry = default;
            Entry[]? entries = _entries;
            if (entries is null)
            {
                // Re-entrancy guard: building the map issues reads through the same fallback path.
                // Those reads target captured CLR structures (no image needed); if one recurses
                // here, bail out rather than looping.
                if (_building || Target is null)
                    return false;

                _building = true;
                try
                {
                    entries = Build(Target);
                }
                catch
                {
                    entries = [];
                }
                finally
                {
                    _building = false;
                }
                _entries = entries;
            }

            foreach (Entry e in entries)
            {
                if (e.Size != 0 && address >= e.Base && address < e.Base + e.Size)
                {
                    entry = e;
                    return true;
                }
            }
            return false;
        }

        private static Entry[] Build(ContractDescriptorTarget target)
        {
            ILoader loader = target.Contracts.Loader;
            TargetPointer appDomain = loader.GetAppDomain();
            List<Entry> entries = [];
            foreach (Microsoft.Diagnostics.DataContractReader.Contracts.ModuleHandle handle in loader.GetModuleHandles(
                appDomain, AssemblyIterationFlags.IncludeLoaded | AssemblyIterationFlags.IncludeExecution))
            {
                if (!loader.TryGetLoadedImageContents(handle, out TargetPointer baseAddress, out uint size, out uint flags))
                    continue;

                string simpleName;
                try { simpleName = loader.GetSimpleName(handle); }
                catch { continue; }

                if (!string.IsNullOrEmpty(simpleName) && baseAddress != TargetPointer.Null && size != 0)
                    entries.Add(new Entry(baseAddress.Value, size, flags, simpleName));
            }
            return entries.ToArray();
        }
    }

    // Directories to search for on-disk assemblies backing dump modules. Set CDAC_IMAGE_PATH
    // (path-separator delimited) to the app bundle / publish directory when analyzing a dump whose
    // image content was omitted (the real-minidump case).
    private static readonly string[] s_imageSearchPaths =
        (Environment.GetEnvironmentVariable("CDAC_IMAGE_PATH") ?? "")
            .Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

    private static string? FindAssemblyImage(string simpleName)
    {
        foreach (string dir in s_imageSearchPaths)
        {
            string candidate = Path.Combine(dir, simpleName + ".dll");
            if (File.Exists(candidate))
                return candidate;
        }
        return null;
    }

    /// <summary>
    /// Reads memory from the dump, falling back to the on-disk PE image when the dump
    /// does not contain the requested bytes. Minidumps (and the DAC's Normal dumps) omit
    /// most module content — R2R code/metadata in particular — so the reader must re-read
    /// it from the module file. Returns 0 on success, -1 on failure.
    /// </summary>
    private static int ReadWithImageFallback(DataTarget dt, LoaderImageMap imageMap, ulong address, Span<byte> buffer)
    {
        try
        {
            int bytesRead = dt.DataReader.Read(address, buffer);
            if (bytesRead == buffer.Length)
                return 0;

            // Prefer the cDAC Loader module map (covers managed PE assemblies that ClrMD's Mach-O
            // module enumeration misses); fall back to ClrMD's module list for native images.
            ulong imageBase;
            string? foundFile;
            bool isMappedLayout;
            if (imageMap.TryFind(address, out LoaderImageMap.Entry entry))
            {
                imageBase = entry.Base;
                foundFile = FindAssemblyImage(entry.SimpleName);
                isMappedLayout = (entry.Flags & 0x1) != 0; // PEImageLayout FLAG_MAPPED
            }
            else
            {
                ModuleInfo? info = GetModuleForAddress(dt, address);
                if (info?.FileName is null)
                    return -1;
                imageBase = info.ImageBase;
                foundFile = FindFileOnDisk(info.FileName);
                isMappedLayout = true; // ClrMD reports loaded (mapped) native images
            }

            if (foundFile is null)
                return -1;

            using FileStream fs = File.OpenRead(foundFile);
            using PEReader peReader = new(fs);

            // Flat layout (the runtime mapped the raw file image, e.g. R2R on mobile): the offset
            // from the image base is a file offset, so serve the on-disk bytes directly. Mapped
            // layout: the offset is an RVA, so translate through the section table.
            if (!isMappedLayout)
            {
                PEMemoryBlock image = peReader.GetEntireImage();
                int fileOffset = (int)(address - imageBase);
                if (fileOffset < 0 || fileOffset + (buffer.Length - bytesRead) > image.Length)
                    return -1;
                unsafe
                {
                    new ReadOnlySpan<byte>(image.Pointer + fileOffset + bytesRead, buffer.Length - bytesRead)
                        .CopyTo(buffer.Slice(bytesRead));
                }
                return 0;
            }

            int sizeOfHeaders = peReader.PEHeaders.PEHeader?.SizeOfHeaders ?? 0;
            PEMemoryBlock wholeImage = default;
            bool wholeImageLoaded = false;

            int filled = bytesRead;
            ulong current = address + (ulong)bytesRead;
            while (filled < buffer.Length)
            {
                int rva = (int)(current - imageBase);
                PEMemoryBlock block = peReader.GetSectionData(rva);
                if (block.Length > 0)
                {
                    int toCopy = Math.Min(block.Length, buffer.Length - filled);
                    unsafe
                    {
                        new ReadOnlySpan<byte>(block.Pointer, toCopy).CopyTo(buffer.Slice(filled));
                    }
                    filled += toCopy;
                    current += (ulong)toCopy;
                }
                else if (rva >= 0 && rva < sizeOfHeaders)
                {
                    // PE header region (before the first section): GetSectionData doesn't cover it.
                    // For a loaded image the headers sit at file offset == RVA, so serve them from
                    // the raw file image. Needed to read a module's PE/COR headers to locate ECMA
                    // metadata when the headers aren't captured in the dump.
                    if (!wholeImageLoaded)
                    {
                        wholeImage = peReader.GetEntireImage();
                        wholeImageLoaded = true;
                    }
                    int available = Math.Min(sizeOfHeaders, wholeImage.Length) - rva;
                    int toCopy = Math.Min(available, buffer.Length - filled);
                    if (toCopy <= 0)
                        return -1;
                    unsafe
                    {
                        new ReadOnlySpan<byte>(wholeImage.Pointer + rva, toCopy).CopyTo(buffer.Slice(filled));
                    }
                    filled += toCopy;
                    current += (ulong)toCopy;
                }
                else
                {
                    return -1;
                }
            }

            return 0;
        }
        catch
        {
            return -1;
        }
    }

    private static ModuleInfo? GetModuleForAddress(DataTarget dt, ulong address)
    {
        foreach (ModuleInfo module in dt.DataReader.EnumerateModules())
        {
            if (address >= module.ImageBase && address < module.ImageBase + (ulong)module.ImageSize)
                return module;
        }

        return null;
    }

    private static string? FindFileOnDisk(string modulePath)
    {
        // For local runs the path recorded in the dump usually still exists on disk.
        if (File.Exists(modulePath))
            return modulePath;

        // Otherwise look next to the executing tool (module file copied alongside).
        int lastSep = Math.Max(modulePath.LastIndexOf('/'), modulePath.LastIndexOf('\\'));
        string fileName = lastSep >= 0 ? modulePath[(lastSep + 1)..] : modulePath;
        string candidate = Path.Combine(AppContext.BaseDirectory, fileName);
        return File.Exists(candidate) ? candidate : null;
    }
}
