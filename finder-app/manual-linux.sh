#!/bin/sh
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
# CROSS_COMPILE=aarch64-none-linux-gnu-

if command -v aarch64-none-linux-gnu-gcc >/dev/null 2>&1 && \
   command -v aarch64-none-linux-gnu-ld >/dev/null 2>&1; then

    CROSS_COMPILE=aarch64-none-linux-gnu-

elif command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 && \
     command -v aarch64-linux-gnu-ld >/dev/null 2>&1; then

    CROSS_COMPILE=aarch64-linux-gnu-

else
    echo "ERROR: Complete AArch64 cross toolchain not found"
    exit 1
fi

echo "Using cross compiler: ${CROSS_COMPILE}"


if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p "${OUTDIR}"
OUTDIR=$(realpath "${OUTDIR}")

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi


if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}
    echo "Clean old kernel (if any)"
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper
    echo "Set default kernel"
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    echo "Compile kernel"
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} Image
fi

echo "Adding the Image in outdir"

echo "Copy actually the kernel..."
cp ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ${OUTDIR}/Image


echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

echo "Creating necessary base directories"
mkdir -p ${OUTDIR}/rootfs
mkdir -p ${OUTDIR}/rootfs/bin
mkdir -p ${OUTDIR}/rootfs/dev
mkdir -p ${OUTDIR}/rootfs/etc
mkdir -p ${OUTDIR}/rootfs/home
mkdir -p ${OUTDIR}/rootfs/lib
mkdir -p ${OUTDIR}/rootfs/lib64
mkdir -p ${OUTDIR}/rootfs/proc
mkdir -p ${OUTDIR}/rootfs/sbin
mkdir -p ${OUTDIR}/rootfs/sys
mkdir -p ${OUTDIR}/rootfs/tmp
mkdir -p ${OUTDIR}/rootfs/usr/bin
mkdir -p ${OUTDIR}/rootfs/usr/lib
mkdir -p ${OUTDIR}/rootfs/usr/sbin
mkdir -p ${OUTDIR}/rootfs/var/log



cd "${OUTDIR}"

if [ ! -d "${OUTDIR}/busybox" ]; then
    git clone https://git.busybox.net/busybox
fi

cd "${OUTDIR}/busybox"

git checkout ${BUSYBOX_VERSION}

echo "Configuring BusyBox"
make distclean
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig

echo "Disabling BusyBox tc applet"
sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config

echo "Checking tc configuration"
grep 'CONFIG_TC' .config

echo "Building BusyBox"
make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}

echo "Install BusyBox into FHS rootfs"
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} \
    CONFIG_PREFIX=${OUTDIR}/rootfs install

echo "Library dependencies"
${CROSS_COMPILE}readelf -a busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a busybox | grep "Shared library"

echo "Adding ARM64 library dependencies to rootfs"

LIBC_PATH=$(${CROSS_COMPILE}gcc -print-file-name=libc.so.6)
LIBM_PATH=$(${CROSS_COMPILE}gcc -print-file-name=libm.so.6)
LIBRESOLV_PATH=$(${CROSS_COMPILE}gcc -print-file-name=libresolv.so.2)
LOADER_PATH=$(${CROSS_COMPILE}gcc -print-file-name=ld-linux-aarch64.so.1)

echo "libc:      ${LIBC_PATH}"
echo "libm:      ${LIBM_PATH}"
echo "libresolv: ${LIBRESOLV_PATH}"
echo "loader:    ${LOADER_PATH}"

for lib in "${LOADER_PATH}" "${LIBC_PATH}" "${LIBM_PATH}" "${LIBRESOLV_PATH}"
do
    if [ ! -f "${lib}" ]; then
        echo "ERROR: Required ARM64 library not found: ${lib}"
        exit 1
    fi
done

cp -L "${LOADER_PATH}" "${OUTDIR}/rootfs/lib/"
cp -L "${LIBC_PATH}" "${OUTDIR}/rootfs/lib/"
cp -L "${LIBM_PATH}" "${OUTDIR}/rootfs/lib/"
cp -L "${LIBRESOLV_PATH}" "${OUTDIR}/rootfs/lib/"



echo "Create device nodes"
sudo mknod -m 666 ${OUTDIR}/rootfs/dev/null c 1 3
sudo mknod -m 600 ${OUTDIR}/rootfs/dev/console c 5 1


echo "Cleanand cross-compile writer"
cd ${FINDER_APP_DIR}
make clean
make CROSS_COMPILE=${CROSS_COMPILE}


cp writer ${OUTDIR}/rootfs/home/
echo "Copying finder scripts and configuration"

cp writer.sh ${OUTDIR}/rootfs/home/
chmod +x ${OUTDIR}/rootfs/home/writer.sh
cp finder.sh ${OUTDIR}/rootfs/home/
cp finder-test.sh ${OUTDIR}/rootfs/home/
cp autorun-qemu.sh ${OUTDIR}/rootfs/home/

mkdir -p ${OUTDIR}/rootfs/home/conf

cp ${FINDER_APP_DIR}/../conf/username.txt \
    ${OUTDIR}/rootfs/home/conf/

cp ${FINDER_APP_DIR}/../conf/assignment.txt \
    ${OUTDIR}/rootfs/home/conf/


sed -i 's#../conf/assignment.txt#conf/assignment.txt#g' \
    ${OUTDIR}/rootfs/home/finder-test.sh


echo "Changing rootfs ownership to root"
sudo chown -R root:root ${OUTDIR}/rootfs


echo "Creating initramfs"

cd ${OUTDIR}/rootfs

find . | cpio -H newc -o | gzip > ${OUTDIR}/initramfs.cpio.gz