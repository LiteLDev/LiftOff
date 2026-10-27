# Minimal Bedrock level.dat (little-endian NBT) reader/writer, used to build and inspect
# world fixtures. Dot-source it, then:
#
#   $ld = Read-LevelDat 'path\level.dat'
#   $ld.Root['LevelName'].Value = 'LO-Test'
#   Set-NbtValue $ld.Root 'doDaylightCycle' 1 0     # (compound, name, tagType, value): adds or replaces
#   Write-LevelDat $ld 'path\level.dat'
#   Show-Nbt $ld.Root                                # dump for inspection
#
# Tag types: 1 byte, 2 short, 3 int, 4 long, 5 float, 6 double, 7 byte[], 8 string,
# 9 list, 10 compound, 11 int[], 12 long[].

if (-not ('BsE2E.Nbt' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
namespace BsE2E {
public class NbtTag {
    public byte Type;
    public object Value;        // primitives, string, arrays, NbtCompound, NbtList
    public NbtTag(byte t, object v) { Type = t; Value = v; }
    public override string ToString() { return Value == null ? "null" : Value.ToString(); }
}
public class NbtCompound {
    public List<string> Keys = new List<string>();
    public Dictionary<string, NbtTag> Map = new Dictionary<string, NbtTag>();
    public NbtTag this[string k] {
        get { NbtTag t; return Map.TryGetValue(k, out t) ? t : null; }
        set { if (!Map.ContainsKey(k)) Keys.Add(k); Map[k] = value; }
    }
    public bool Remove(string k) { Keys.Remove(k); return Map.Remove(k); }
    public override string ToString() { return "{compound " + Keys.Count + "}"; }
}
public class NbtList {
    public byte ElemType;
    public List<object> Items = new List<object>();
    public override string ToString() { return "[list " + Items.Count + "]"; }
}
public class LevelDat {
    public int StorageVersion;
    public string RootName = "";
    public NbtCompound Root;
}
public static class Nbt {
    public static LevelDat ReadLevelDat(byte[] data) {
        var ld = new LevelDat();
        ld.StorageVersion = BitConverter.ToInt32(data, 0);
        var r = new BinaryReader(new MemoryStream(data, 8, data.Length - 8));
        byte t = r.ReadByte();
        if (t != 10) throw new InvalidDataException("root is not a compound");
        ld.RootName = ReadString(r);
        ld.Root = (NbtCompound)ReadPayload(r, 10);
        return ld;
    }
    public static byte[] WriteLevelDat(LevelDat ld) {
        var ms = new MemoryStream();
        var w = new BinaryWriter(ms);
        w.Write((byte)10); WriteString(w, ld.RootName); WritePayload(w, 10, ld.Root);
        w.Flush();
        byte[] body = ms.ToArray();
        byte[] outp = new byte[body.Length + 8];
        BitConverter.GetBytes(ld.StorageVersion).CopyTo(outp, 0);
        BitConverter.GetBytes(body.Length).CopyTo(outp, 4);
        body.CopyTo(outp, 8);
        return outp;
    }
    static string ReadString(BinaryReader r) { int n = r.ReadUInt16(); return Encoding.UTF8.GetString(r.ReadBytes(n)); }
    static void WriteString(BinaryWriter w, string s) { var b = Encoding.UTF8.GetBytes(s); w.Write((ushort)b.Length); w.Write(b); }
    static object ReadPayload(BinaryReader r, byte t) {
        switch (t) {
            case 1: return r.ReadSByte();
            case 2: return r.ReadInt16();
            case 3: return r.ReadInt32();
            case 4: return r.ReadInt64();
            case 5: return r.ReadSingle();
            case 6: return r.ReadDouble();
            case 7: { int n = r.ReadInt32(); return r.ReadBytes(n); }
            case 8: return ReadString(r);
            case 9: {
                var l = new NbtList(); l.ElemType = r.ReadByte(); int n = r.ReadInt32();
                for (int i = 0; i < n; i++) l.Items.Add(ReadPayload(r, l.ElemType));
                return l;
            }
            case 10: {
                var c = new NbtCompound();
                while (true) {
                    byte ct = r.ReadByte();
                    if (ct == 0) break;
                    string name = ReadString(r);
                    c[name] = new NbtTag(ct, ReadPayload(r, ct));
                }
                return c;
            }
            case 11: { int n = r.ReadInt32(); var a = new int[n]; for (int i = 0; i < n; i++) a[i] = r.ReadInt32(); return a; }
            case 12: { int n = r.ReadInt32(); var a = new long[n]; for (int i = 0; i < n; i++) a[i] = r.ReadInt64(); return a; }
        }
        throw new InvalidDataException("unknown tag type " + t);
    }
    static void WritePayload(BinaryWriter w, byte t, object v) {
        switch (t) {
            case 1: w.Write(Convert.ToSByte(v)); return;
            case 2: w.Write(Convert.ToInt16(v)); return;
            case 3: w.Write(Convert.ToInt32(v)); return;
            case 4: w.Write(Convert.ToInt64(v)); return;
            case 5: w.Write(Convert.ToSingle(v)); return;
            case 6: w.Write(Convert.ToDouble(v)); return;
            case 7: { var b = (byte[])v; w.Write(b.Length); w.Write(b); return; }
            case 8: WriteString(w, (string)v); return;
            case 9: {
                var l = (NbtList)v; w.Write(l.ElemType); w.Write(l.Items.Count);
                foreach (var it in l.Items) WritePayload(w, l.ElemType, it);
                return;
            }
            case 10: {
                var c = (NbtCompound)v;
                foreach (var k in c.Keys) { var tag = c.Map[k]; w.Write(tag.Type); WriteString(w, k); WritePayload(w, tag.Type, tag.Value); }
                w.Write((byte)0);
                return;
            }
            case 11: { var a = (int[])v; w.Write(a.Length); foreach (var x in a) w.Write(x); return; }
            case 12: { var a = (long[])v; w.Write(a.Length); foreach (var x in a) w.Write(x); return; }
        }
        throw new InvalidDataException("unknown tag type " + t);
    }
}
}
'@
}

function Read-LevelDat([string]$Path) { [BsE2E.Nbt]::ReadLevelDat([IO.File]::ReadAllBytes($Path)) }

function Write-LevelDat($LevelDat, [string]$Path) { [IO.File]::WriteAllBytes($Path, [BsE2E.Nbt]::WriteLevelDat($LevelDat)) }

# Add or replace a tag in a compound. $Type is the NBT tag type (see header).
function Set-NbtValue([BsE2E.NbtCompound]$Compound, [string]$Name, [byte]$Type, $Value) {
    $Compound[$Name] = [BsE2E.NbtTag]::new($Type, $Value)
}

function Show-Nbt($Value, [string]$Indent = '') {
    if ($Value -is [BsE2E.NbtCompound]) {
        foreach ($k in $Value.Keys) {
            $t = $Value[$k]
            if ($t.Value -is [BsE2E.NbtCompound] -or $t.Value -is [BsE2E.NbtList]) {
                Write-Output "$Indent$k ($($t.Type)):"; Show-Nbt $t.Value "$Indent  "
            } else { Write-Output "$Indent$k ($($t.Type)) = $($t.Value)" }
        }
    } elseif ($Value -is [BsE2E.NbtList]) {
        $i = 0
        foreach ($it in $Value.Items) {
            if ($it -is [BsE2E.NbtCompound]) { Write-Output "$Indent[$i]:"; Show-Nbt $it "$Indent  " } else { Write-Output "$Indent[$i] = $it" }
            $i++
        }
    }
}
