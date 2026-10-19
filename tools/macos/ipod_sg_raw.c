/*
 * Status: rough first version. It works (it was used for all device access while
 * developing the A1717 patch), but it still needs cleanup and more input checks.
 * It is meant to go upstream into NanoApps later; until then it lives here, and
 * nanoapps-macos-transport.patch (same folder) adds macOS support to NanoApps' start
 * script. See the README section "macOS SCSI tool and NanoApps".
 */
/* ipod_sg_raw — sg_raw-compatible raw SCSI for iPods on macOS.
 *
 * Uses Apple's SCSITaskDeviceInterface (QA1179), which macOS exposes for iPods
 * via com_apple_driver_iPodSBCNub (SCSITaskDeviceCategory = iPodUserClientDevice).
 *
 * Usage (subset of sg_raw):
 *   ipod_sg_raw [-s N -i INFILE] [-o OUTFILE -r N] [-t TIMEOUT_MS] [-v] DEV CDB_HEX...
 * DEV is accepted for compatibility and ignored (the single attached iPod is used).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/scsi/SCSITaskLib.h>
#include <IOKit/scsi/SCSICommandOperationCodes.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int verbose = 0;

static void die(const char *m, IOReturn r) {
    fprintf(stderr, "ipod_sg_raw: %s (0x%08x)\n", m, r);
    exit(2);
}

int main(int argc, char **argv) {
    const char *infile = NULL, *outfile = NULL;
    long slen = 0, rlen = 0; UInt32 timeout = 10000;
    int opt;
    while ((opt = getopt(argc, argv, "s:i:o:r:t:v")) != -1) {
        switch (opt) {
        case 's': slen = strtol(optarg, 0, 0); break;
        case 'i': infile = optarg; break;
        case 'o': outfile = optarg; break;
        case 'r': rlen = strtol(optarg, 0, 0); break;
        case 't': timeout = (UInt32)strtoul(optarg, 0, 0); break;
        case 'v': verbose++; break;
        default: fprintf(stderr, "usage: %s [-s N -i in] [-o out -r N] [-t ms] DEV CDB...\n", argv[0]); return 1;
        }
    }
    if (argc - optind < 2) { fprintf(stderr, "need DEV and CDB bytes\n"); return 1; }
    optind++; /* skip DEV */
    UInt8 cdb[16] = {0}; UInt8 cdblen = 0;
    for (; optind < argc && cdblen < 16; optind++) cdb[cdblen++] = (UInt8)strtoul(argv[optind], 0, 16);

    /* data buffer */
    long len = slen ? slen : rlen;
    UInt8 dir = slen ? kSCSIDataTransfer_FromInitiatorToTarget
              : rlen ? kSCSIDataTransfer_FromTargetToInitiator
              : kSCSIDataTransfer_NoDataTransfer;
    UInt8 *buf = NULL;
    if (len) {
        buf = valloc(len); memset(buf, 0, len);
        if (slen) {
            int fd = open(infile, O_RDONLY); if (fd < 0) { perror(infile); return 1; }
            ssize_t n = read(fd, buf, len); close(fd);
            if (n < 0) { perror("read"); return 1; }
        }
    }

    /* find the iPod nub that carries the SCSITaskUserClient */
    io_iterator_t it; IOReturn kr;
    kr = IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("com_apple_driver_iPodSBCNub"), &it);
    if (kr) die("IOServiceGetMatchingServices", kr);
    io_service_t svc = IOIteratorNext(it); IOObjectRelease(it);
    if (!svc) { fprintf(stderr, "ipod_sg_raw: no iPod (com_apple_driver_iPodSBCNub) found\n"); return 2; }

    IOCFPlugInInterface **plug = NULL; SInt32 score;
    kr = IOCreatePlugInInterfaceForService(svc, kIOSCSITaskDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(svc);
    if (kr || !plug) die("IOCreatePlugInInterfaceForService", kr);

    SCSITaskDeviceInterface **dev = NULL;
    HRESULT hr = (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOSCSITaskDeviceInterfaceID), (LPVOID *)&dev);
    if (hr || !dev) die("QueryInterface SCSITaskDeviceInterface", (IOReturn)hr);

    if (!(*dev)->IsExclusiveAccessAvailable(dev) && verbose)
        fprintf(stderr, "ipod_sg_raw: exclusive access not available (volume mounted?)\n");
    kr = (*dev)->ObtainExclusiveAccess(dev);
    if (kr) die("ObtainExclusiveAccess (unmount the iPod volume first: diskutil unmountDisk <disk>)", kr);

    SCSITaskInterface **task = (*dev)->CreateSCSITask(dev);
    if (!task) die("CreateSCSITask", 0);

    /* SCSITaskLib only accepts 6/10/12/16-byte CDBs; sg_raw sends any length (the
     * iPod's vendor c6 96 .. commands are 7 bytes). Pad with zeros to the next valid
     * size -- vendor-group opcodes carry no fixed length, the device ignores the tail. */
    { UInt8 valid[] = {6, 10, 12, 16}; for (int i = 0; i < 4; i++) if (cdblen <= valid[i]) { cdblen = valid[i]; break; } }
    kr = (*task)->SetCommandDescriptorBlock(task, cdb, cdblen);
    if (kr) die("SetCommandDescriptorBlock", kr);
    SCSITaskSGElement sg = { (mach_vm_address_t)buf, (mach_vm_size_t)len };
    kr = (*task)->SetScatterGatherEntries(task, len ? &sg : NULL, len ? 1 : 0, (UInt64)len, dir);
    if (kr) die("SetScatterGatherEntries", kr);
    (*task)->SetTimeoutDuration(task, timeout);

    SCSI_Sense_Data sense; memset(&sense, 0, sizeof sense);
    SCSITaskStatus status = 0xff; UInt64 realized = 0;
    kr = (*task)->ExecuteTaskSync(task, &sense, &status, &realized);

    if (verbose || kr || status != kSCSITaskStatus_GOOD)
        fprintf(stderr, "ipod_sg_raw: cdb=%02x%s status=0x%02x kr=0x%08x realized=%llu sense=%02x/%02x/%02x\n",
                cdb[0], cdblen > 1 ? "…" : "", status, kr, realized,
                sense.SENSE_KEY & 0x0f, sense.ADDITIONAL_SENSE_CODE, sense.ADDITIONAL_SENSE_CODE_QUALIFIER);

    int rc = (kr == kIOReturnSuccess && status == kSCSITaskStatus_GOOD) ? 0 : 3;
    if (rlen && outfile) {
        int fd = strcmp(outfile, "-") ? open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644) : 1;
        if (fd < 0) perror(outfile); else { write(fd, buf, rlen); if (fd != 1) close(fd); }
    }
    (*task)->Release(task);
    (*dev)->ReleaseExclusiveAccess(dev);
    (*dev)->Release(dev);
    IODestroyPlugInInterface(plug);
    free(buf);
    return rc;
}
