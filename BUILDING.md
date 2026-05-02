# Building Gradient from source (host side: Linux)

`Gradient` is a regular `vbcc` cross-compile to AmigaOS 3.x Hunk format.
The toolchain (`vbcc` + `vasm` + `vlink` + NDK) lives entirely under
`toolchain/` inside this project directory; nothing global is installed.
The whole tree is gitignored, so each fresh clone needs a one-time build.

The setup takes ~5 minutes on a modern Linux box and ~250 MB of disk.

---

## 1. Host requirements

Debian / Ubuntu / similar:

```
sudo apt-get install -y build-essential lhasa wget unzip file
```

`build-essential` to build the toolchain itself, `lhasa` to unpack `.lha`
archives (vbcc target + NDK), `wget` to fetch the sources.

## 2. Build the toolchain into ./toolchain/

Run these from the project root (where `gradient.c` lives):

```sh
mkdir -p toolchain/{src,bin,targets,downloads,ndk}
cd toolchain/downloads

# vasm (assembler), vlink (linker), vbcc (compiler), and the AmigaOS
# m68k target archive (config + libs).
wget http://sun.hasenbraten.de/vasm/release/vasm.tar.gz
wget http://sun.hasenbraten.de/vlink/release/vlink.tar.gz
wget http://phoenix.owl.de/tags/vbcc0_9hP1.tar.gz
wget http://phoenix.owl.de/vbcc/2022-05-22/vbcc_target_m68k-amigaos.lha

# NDK 3.2 (Hyperion's modern reissue; works fine for OS 3.1).
wget http://aminet.net/dev/misc/NDK3.2.lha

cd ../src
tar xf ../downloads/vasm.tar.gz
tar xf ../downloads/vlink.tar.gz
tar xf ../downloads/vbcc0_9hP1.tar.gz

# vasm
cd vasm && make CPU=m68k SYNTAX=mot
cp vasmm68k_mot vobjdump ../../bin/
cd ..

# vlink
cd vlink && make
cp vlink ../../bin/
cd ..

# vbcc
cd vbcc && mkdir -p bin && yes '' | make TARGET=m68k
cp bin/{vbccm68k,vc,vprof,dtgen} ../../bin/
cd ../..

# vbcc target (config + libs + minimal headers)
lha x downloads/vbcc_target_m68k-amigaos.lha
cp -r vbcc_target_m68k-amigaos/config ./
cp -r vbcc_target_m68k-amigaos/targets/m68k-amigaos targets/
rm -rf vbcc_target_m68k-amigaos vbcc_target_m68k-amigaos.info

# NDK headers + libs (merge into the vbcc target)
mkdir -p ndk
cd ndk && lha xq ../downloads/NDK3.2.lha
cd ..
cp -rn ndk/Include_H/* targets/m68k-amigaos/include/

# Patch the vbcc config to use absolute paths (default uses AmigaDOS-style
# assigns like vincludeos3: which need real paths on Linux), and replace
# AmigaDOS `delete` with `rm -f`.
PROJECT="$(cd .. && pwd)"
for f in config/aos68k config/aos68km config/aos68kr; do
    sed -i \
        -e "s|vincludeos3:|$PROJECT/toolchain/targets/m68k-amigaos/include/|g" \
        -e "s|vlibos3:|$PROJECT/toolchain/targets/m68k-amigaos/lib/|g" \
        -e 's|^-rm=delete quiet|-rm=rm -f|' \
        -e 's|^-rmv=delete|-rmv=rm -fv|' \
        "$f"
done

cd ..
```

After the script above finishes, you should have:

```
toolchain/
├── bin/                                  vasmm68k_mot, vlink, vbcc, vc, ...
├── config/aos68k                         (and aos68km, aos68kr)
├── targets/m68k-amigaos/{include,lib}    headers + amiga.lib + vbcc libs
├── ndk/                                  unpacked NDK 3.2 (full)
├── src/                                  source build trees
└── downloads/                            tarballs + .lha
```

## 3. Build Gradient

From the project root:

```
. ./env.sh         # exports VBCC, PATH, VINCLUDEOS3, VLIBOS3
make Gradient
```

Verify:

```
file Gradient
# -> Gradient: AmigaOS loadseg()ble executable/binary

strings Gradient | grep '$VER:'
# -> $VER: Gradient 1.0
```

Copy `Gradient` to the Amiga (CF, ADF, serial, fs-uae shared folder, ...).

## 4. Notes

- The patched paths in `config/aos68k` are absolute — if you move the
  project directory you have to re-patch. Easiest: re-run the `sed` block
  in step 2 with the new `$PROJECT` value.
- `toolchain/downloads/` and `toolchain/ndk/` can be deleted after build
  if you want to save disk; you'd just need to re-fetch on a future
  rebuild.
- Upstream URLs occasionally drift. If `phoenix.owl.de` or
  `sun.hasenbraten.de` is down, try Aminet:
  `aminet.net/dev/c/vbcc_bin_amigaos68k.lha` and equivalents.
