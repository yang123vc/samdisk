// Platform specific HDD device handling

#include "SAMdisk.h"
#include "DeviceHDD.h"

// ToDo: split conditional code into separate classes

DeviceHDD::DeviceHDD ()
	: hdev(INVALID_HANDLE_VALUE)
{
}

DeviceHDD::~DeviceHDD ()
{
	Unlock();
}

bool DeviceHDD::Open (const std::string &path)
{
	// Open as read-write, falling back on read-only
	if ((h = open(path.c_str(), O_RDWR | O_SEQUENTIAL | O_BINARY)) == -1 &&
		(h = open(path.c_str(), O_RDONLY | O_SEQUENTIAL | O_BINARY)) == -1)
	{
// Win32 has a second attempt at opening, via SAMdiskHelper
#ifdef _WIN32
#define PIPENAME	R"(\\.\pipe\SAMdiskHelper)"
#define FN_OPEN		2

#pragma pack(push,1)
		typedef struct
		{
			union
			{
				struct
				{
					DWORD dwMessage;
					char szPath[MAX_PATH];
				} Input;

				struct
				{
					DWORD dwError;
					DWORD64 hDevice;
				} Output;
			};
		} PIPEMESSAGE;
#pragma pack(pop)

		DWORD dwRead;
		PIPEMESSAGE msg = {};
		msg.Input.dwMessage = FN_OPEN;
		strncpy(msg.Input.szPath, path.c_str(), MAX_PATH - 1);

		if (CallNamedPipe(PIPENAME, &msg, sizeof(msg.Input), &msg, sizeof(msg.Output), &dwRead, NMPWAIT_NOWAIT))
		{
			if (dwRead == sizeof(msg.Output) && msg.Output.dwError == 0)
			{
				// Wrap the Win32 handler in a CRT file handle
				h = _open_osfhandle((intptr_t)msg.Output.hDevice, 0);
			}
			else if (msg.Output.dwError != 0)
			{
				SetLastError(msg.Output.dwError);
			}
		}

		if (h == -1)
#endif
		{
			return false;
		}
	}

#ifdef _WIN32
	DWORD dwRet;

	// Retrieve the Win32 file handle for IOCTL calls
	hdev = reinterpret_cast<HANDLE>(_get_osfhandle(h));

	// Determine sector size
	DISK_GEOMETRY dg;
	if (DeviceIoControl(hdev, IOCTL_DISK_GET_DRIVE_GEOMETRY, nullptr, 0, &dg, sizeof(dg), &dwRet, nullptr) && dg.BytesPerSector)
		sector_size = dg.BytesPerSector;
	else
		sector_size = SECTOR_SIZE;

	// Determine byte size, and from that calculate sector count
	PARTITION_INFORMATION pi;
	if (DeviceIoControl(hdev, IOCTL_DISK_GET_PARTITION_INFO, nullptr, 0, &pi, sizeof(pi), &dwRet, nullptr))
	{
		total_bytes = pi.PartitionLength.QuadPart;

		// HACK: round to an even number of sectors, to fix broken CF cards/readers
		total_bytes &= ~0x3ff;

		total_sectors = total_bytes / sector_size;

#if 0
		// ToDo: still needed?
		// Rather than clip to 32-bits, use the maximum value
		if (total_bytes > total_sectors * sector_size)
			total_sectors = (1LL << 32) - 1;
#endif
	}
#elif defined(BLKGETSIZE)
	long lSize;
	if (ioctl(h, BLKGETSIZE, &lSize) == 0)
	{
		sector_size = SECTOR_SIZE;
		total_bytes = lSize * SECTOR_SIZE; // always 512-byte units
		total_sectors = lSize;
		total_bytes = total_sectors * SECTOR_SIZE; // always 512-byte units
	}
#elif defined(HAVE_DISKARBITRATION_DISKARBITRATION_H)
	DASessionRef session = DASessionCreate(kCFAllocatorDefault);
	DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, path.c_str());
	if (disk)
	{
		CFDictionaryRef diskInfo = DADiskCopyDescription(disk);
		if (diskInfo)
		{
			long long llSize = 0;
			int nBlockSize = SECTOR_SIZE;

			auto numMediaSize = static_cast<CFNumberRef>(CFDictionaryGetValue(diskInfo, kDADiskDescriptionMediaSizeKey));
			auto numBlockSize = static_cast<CFNumberRef>(CFDictionaryGetValue(diskInfo, kDADiskDescriptionMediaBlockSizeKey));

			CFNumberGetValue(numMediaSize, kCFNumberLongLongType, &llSize);
			CFNumberGetValue(numBlockSize, kCFNumberIntType, &nBlockSize);

			sector_size = nBlockSize;
			total_bytes = llSize;
			total_sectors = total_bytes / sector_size;

			CFRelease(diskInfo);
		}

		CFRelease(disk);
		CFRelease(session);
	}
#endif // WIN32
	else
	{
#ifdef _WIN32
		struct _stat64 st;
		if (_stati64(path.c_str(), &st))
		{
			SetLastError(ERROR_ACCESS_DENIED);
			return false;
		}
#else
		struct stat st;
		if (stat(path.c_str(), &st))
			return false;
#endif
		sector_size = SECTOR_SIZE;
		total_bytes = st.st_size - data_offset;
		total_sectors = static_cast<unsigned>(total_bytes / sector_size);
	}

	// Reject devices without media
	if (!total_sectors)
		return false;

	// Read the real identify data if possible
	if (ReadIdentifyData(hdev, sIdentify))
		SetIdentifyData(&sIdentify);

	// If we lack CHS values from the identify data, calculate suitable values from the total sector count
	if (!sectors)
		CalculateGeometry(total_sectors, cyls, heads, sectors);

	// Read the make/model, firmware revision and serial number directly, if possible
	ReadMakeModelRevisionSerial(path);

	return true;
}


bool DeviceHDD::Lock ()
{
#ifdef _WIN32
	DWORD dwRet;

	std::vector<std::string> lVolumes = GetVolumeList();

// Logic from: http://www.techtalkz.com/microsoft-device-drivers/250612-lock-entire-disk-physicaldrive-fsctl_lock_volume.html#post983810

	// Open and lock all volumes
	for (auto sVolume : lVolumes)
	{
		std::string sDevice = R"(\\.\)" + sVolume;

		// Open the volume
		HANDLE hdevice = CreateFile(sDevice.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		lLockHandles.push_back(std::make_pair(hdevice, sVolume));

		// Lock the volume to give us exclusive access
		if (!DeviceIoControl(hdevice, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &dwRet, nullptr))
			throw util::exception("failed to lock ", sVolume, " filesystem");
	}

	// Discount volumes
	for (auto p : lLockHandles)
	{
		if (!DeviceIoControl(p.first, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &dwRet, nullptr))
			throw util::exception("failed to dismount ", p.second, " filesystem");
	}
#endif
	return true;
}

void DeviceHDD::Unlock ()
{
#ifdef _WIN32
	DWORD dwRet;

	// Anything to unlock?
	if (lLockHandles.size())
	{
		for (auto p : lLockHandles)
		{
			if (p.first != INVALID_HANDLE_VALUE)
				CloseHandle(p.first);
		}

		lLockHandles.clear();

		// Refresh the disk properties
		DeviceIoControl(hdev, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &dwRet, nullptr);
	}
#endif
}

bool DeviceHDD::SafetyCheck ()
{
#ifdef _WIN32
	// Safety check can be skipped with the force option
	if (opt.force)
		return true;

	std::vector<std::string> lVolumes = GetVolumeList();
	std::string sRemoveList;

	for (size_t i = 0; i < lVolumes.size(); ++i)
	{
		char szVolName[64] = "";

		std::string sVolume = lVolumes.at(i);
		std::string sRoot = sVolume + "\\";

		// Only accept drives with a known filesystem, as drive letters exist for removeable
		// media even if no disk is preset, or if the filesystem is unknown
		if (GetVolumeInformation(sRoot.c_str(), szVolName, sizeof(szVolName), nullptr, nullptr, nullptr, nullptr, 0))
		{
			if (!sRemoveList.empty())
				sRemoveList += ", ";

			if (szVolName[0])
				sRemoveList += sVolume + " [" + szVolName + "]";
			else
				sRemoveList += sVolume;
		}
	}

	if (!sRemoveList.empty())
		util::cout << "These volumes may be removed: " << sRemoveList << '\n';
#endif

	return HDD::SafetyCheck();
}


std::vector<std::string> DeviceHDD::GetVolumeList () const
{
	std::vector<std::string> lVolumes;

#ifdef _WIN32
	STORAGE_DEVICE_NUMBER sdn;
	DWORD dwRet;

	if (DeviceIoControl(hdev, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &sdn, sizeof(sdn), &dwRet, nullptr))
	{
		DWORD dwDrives = GetLogicalDrives();
		char szDrive[] = R"(\\.\X:)";

		for (char i = 0; i < 26; ++i)
		{
			// Skip non-existent drives
			if (!(dwDrives & (1 << i)))
				continue;

			// Form the volume device path
			szDrive[4] = 'A' + i;

			// Open the volume without accessing the drive contents
			HANDLE hdevice = CreateFile(szDrive, 0, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
			if (hdevice != INVALID_HANDLE_VALUE)
			{
				BYTE ab[256];
				PVOLUME_DISK_EXTENTS pvde = reinterpret_cast<PVOLUME_DISK_EXTENTS>(ab);

				// Get the extents of the volume, which may span multiple physical drives
				if (DeviceIoControl(hdevice, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, pvde, sizeof(ab), &dwRet, nullptr))
				{
					// Check each extent against the supplied drive number, and mark any matches
					for (DWORD u = 0; u < pvde->NumberOfDiskExtents; ++u)
						if (pvde->Extents[u].DiskNumber == sdn.DeviceNumber)
							lVolumes.push_back(szDrive + 4);
				}

				CloseHandle(hdevice);
			}
		}
	}
#endif

	return lVolumes;
}

bool DeviceHDD::ReadIdentifyData (HANDLE h_, IDENTIFYDEVICE &identify_)
{
#ifdef _WIN32
	DWORD dwRet = 0;

	// Input is the identify command (0xEC)
	DWORD dwInSize = sizeof(SENDCMDINPARAMS) - 1;
	MEMORY memIn(dwInSize);
	SENDCMDINPARAMS *pIn = reinterpret_cast<SENDCMDINPARAMS *>(memIn.pb);
	pIn->irDriveRegs.bCommandReg = ID_CMD;

	// Output is a buffer for the identify data
	DWORD dwOutSize = sizeof(SENDCMDOUTPARAMS) + IDENTIFY_BUFFER_SIZE - 1;
	MEMORY memOut(dwOutSize);
	SENDCMDOUTPARAMS *pOut = reinterpret_cast<SENDCMDOUTPARAMS *>(memOut.pb);
	pOut->cBufferSize = IDENTIFY_BUFFER_SIZE;

	// Read the identify data buffer, and copy to the caller's buffer
	if (DeviceIoControl(h_, SMART_RCV_DRIVE_DATA, pIn, dwInSize, pOut, dwOutSize, &dwRet, nullptr))
	{
		memset(&identify_, 0, sizeof(identify_));

		identify_.len = std::min(dwRet, static_cast<DWORD>(sizeof(identify_.byte)));
		memcpy(&identify_.byte, pOut->bBuffer, identify_.len);

		return true;
	}
#else
	(void)h_;
#endif // WIN32

#ifdef HAVE_LINUX_HDREG_H
	static struct hd_driveid hd;

	// ATA query
	if (!ioctl(h, HDIO_GET_IDENTITY, &hd))
	{
		identify_.len = std::min(sizeof(hd), sizeof(identify_.byte));
		memcpy(&identify_.byte, &hd, identify_.len);
	}
#else
	(void)identify_;
#endif

	return false;
}

/*static*/ bool DeviceHDD::IsRecognised (const std::string &path)
{
	return IsDeviceHDD(path) || IsFileHDD(path);
}

/*static*/ bool DeviceHDD::IsDeviceHDD (const std::string &path)
{
#ifdef _WIN32
	// Reject if the first byte isn't a digit
	if (!isdigit(static_cast<BYTE>(path[0])))
		return false;

	// Skip over the device number
	char *pszEnd = nullptr;
	static_cast<void>(strtoul(path.c_str(), &pszEnd, 0));

	// Reject if there's anything left except an optional colon
	if (pszEnd[0] && (pszEnd[0] != ':' || pszEnd[1]))
		return false;

#else
	// Fail if it's not a block device
	struct stat st;
	if (stat(path.c_str(), &st) || !S_ISBLK(st.st_mode))
		return false;
#endif

	// Syntax valid
	return true;
}

/*static*/ bool DeviceHDD::IsFileHDD (const std::string &path)
{
#ifdef _WIN32
	struct _stat64 st;
	if (_stati64(path.c_str(), &st))
#else
	struct stat st;
	if (stat(path.c_str(), &st))
#endif
		return false;

	// Reject files under 4MB, and those not an exact sector multiple
	if (st.st_size < 4 * 1024 * 1024 || (st.st_size % SECTOR_SIZE) != 0)
		return false;

	// Appears valid
	return true;
}

/*static*/ std::vector<std::string> DeviceHDD::GetDeviceList ()
{
	std::vector<std::string> vDevices;

#ifdef _WIN32
	for (int i = 0; i < 20; ++i)
	{
		std::string str = util::fmt(R"(\\.\PhysicalDrive%d)", i);
		HANDLE h = CreateFile(str.c_str(), 0, 0, nullptr, OPEN_EXISTING, 0, nullptr);

		if (h != INVALID_HANDLE_VALUE)
		{
			CloseHandle(h);
			vDevices.push_back(util::fmt("%d", i));
		}
	}
#elif defined(__linux__)
	for (int i = 0; i < 16; ++i)
	{
		char sz[32] = {};
		snprintf(sz, sizeof(sz), "/dev/sd%c", 'a' + i);

		struct stat st;
		if (!stat(sz, &st) && S_ISBLK(st.st_mode))
			vDevices.push_back(sz);
	}
#else
	CFMutableDictionaryRef matchingDict = IOServiceMatching(kIOMediaClass);
	if (matchingDict != nullptr)
	{
		CFDictionarySetValue(matchingDict, CFSTR(kIOMediaWholeKey), kCFBooleanTrue);    // whole disk objects only

		io_iterator_t iter;
		if (IOServiceGetMatchingServices(kIOMasterPortDefault, matchingDict, &iter) == KERN_SUCCESS)
		{
			for (io_object_t nextMedia; (nextMedia = IOIteratorNext(iter)); IOObjectRelease(nextMedia))
			{
				auto deviceRef = static_cast<CFStringRef>(IORegistryEntryCreateCFProperty(nextMedia, CFSTR(kIOBSDNameKey), kCFAllocatorDefault, 0));
				if (!deviceRef)
					continue;

				CFMutableStringRef pathRef = CFStringCreateMutable(kCFAllocatorDefault, 0);
				CFStringAppend(pathRef, CFSTR(_PATH_DEV));
				CFStringAppend(pathRef, deviceRef);

				char bsdPath[PATH_MAX];
				CFStringGetCString(pathRef, bsdPath, PATH_MAX, kCFStringEncodingUTF8);
				vDevices.push_back(bsdPath);

				CFRelease(pathRef);
				CFRelease(deviceRef);
			}
		}
	}
#endif

	// Sort the list and return it
	std::sort(vDevices.begin(), vDevices.end());
	return vDevices;
}


bool DeviceHDD::ReadMakeModelRevisionSerial (const std::string &path)
{
#ifdef _WIN32
	(void)path; // unused
	MEMORY mem(512);
	PSTORAGE_DEVICE_DESCRIPTOR pDevDesc = reinterpret_cast<PSTORAGE_DEVICE_DESCRIPTOR>(mem.pb);
	pDevDesc->Size = static_cast<DWORD>(mem.size);

	STORAGE_PROPERTY_QUERY spq;
	spq.QueryType = PropertyStandardQuery;
	spq.PropertyId = StorageDeviceProperty;

	DWORD dwRet;
	HANDLE hdevice = reinterpret_cast<HANDLE>(_get_osfhandle(this->h));
	if (DeviceIoControl(hdevice, IOCTL_STORAGE_QUERY_PROPERTY, &spq, sizeof(spq), pDevDesc, pDevDesc->Size, &dwRet, nullptr) && pDevDesc->ProductIdOffset)
	{
		// We've read something, so clear existing values
		strMakeModel = strFirmwareRevision = strSerialNumber = "";

		if (pDevDesc->VendorIdOffset)
		{
			std::string s(reinterpret_cast<char*>(pDevDesc) + pDevDesc->VendorIdOffset);
			strMakeModel = util::trim(s);
		}

		if (pDevDesc->ProductIdOffset)
		{
			std::string s(reinterpret_cast<char*>(pDevDesc) + pDevDesc->ProductIdOffset);
			strMakeModel += util::trim(s);
		}

		if (pDevDesc->ProductRevisionOffset)
		{
			std::string s(reinterpret_cast<char*>(pDevDesc) + pDevDesc->ProductRevisionOffset);
			strFirmwareRevision = util::trim(s);
		}

		if (pDevDesc->SerialNumberOffset)
		{
			std::string s(reinterpret_cast<char*>(pDevDesc) + pDevDesc->SerialNumberOffset);
			strSerialNumber = util::trim(s);
		}

		return true;
	}
#endif // WIN32

#ifdef HAVE_LINUX_HDREG_H
	(void)path; // unused
	static struct hd_driveid hd;

	// ATA query
	if (!ioctl(h, HDIO_GET_IDENTITY, &hd))
	{
		std::string s = std::string(reinterpret_cast<const char *>(hd.model), 40);
		strMakeModel = util::trim(s);

		s = std::string(reinterpret_cast<const char *>(hd.serial_no), 20);
		strSerialNumber = util::trim(s);

		s = std::string(reinterpret_cast<const char *>(hd.fw_rev), 8);
		strFirmwareRevision = util::trim(s);

		return true;
	}
#endif // HDIO_GET_IDENTITY

#if defined(HAVE_SCSI_SCSI_H) && defined(HAVE_SCSI_SG_H)
	// SCSI query
	{
		uint8_t buf[96];
		uint8_t cmd[] = { INQUIRY, 0, 0, 0, sizeof(buf), 0 };
		uint8_t sense[32];

		struct sg_io_hdr io_hdr = {};
		io_hdr.interface_id = 'S';
		io_hdr.cmdp = cmd;
		io_hdr.cmd_len = sizeof(cmd);
		io_hdr.dxferp = buf;
		io_hdr.dxfer_len = sizeof(buf);
		io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
		io_hdr.sbp = sense;
		io_hdr.mx_sb_len = sizeof(sense);
		io_hdr.timeout = 5000;

		cmd[1] = 0x00;	// EVPD = 0
		cmd[2] = 0x00;	// Page 00h = Supported vital product data

		// Read the device details
		if (ioctl(h, SG_IO, &io_hdr) == 0 &&
			(io_hdr.info & SG_INFO_OK_MASK) == SG_INFO_OK)
		{
/*
			// Vendor identification (8-15)
			std::string s = std::string(reinterpret_cast<char*>(buf+8), 8);
			strMakeModel = util::trim(s);
*/
			// Product identification (16-31)
			auto s = std::string(reinterpret_cast<char*>(buf + 16), 16);
			strMakeModel = util::trim(s);

			// Product revision level (32-35)
			s = std::string(reinterpret_cast<char*>(buf + 32), 4);
			strFirmwareRevision = util::trim(s);

			cmd[1] = 0x01;	// EVPD = 1
			cmd[2] = 0x80;	// Page 80h = Unit serial number

			// Read the device serial number
			if (ioctl(h, SG_IO, &io_hdr) == 0 &&
				(io_hdr.info & SG_INFO_OK_MASK) == SG_INFO_OK &&
				buf[1] == 0x80) // Page 80h = Unit serial number
			{
				s = std::string(reinterpret_cast<char*>(buf + 4), buf[3]);
				strSerialNumber = util::trim(s);
			}

			return true;
		}
	}
#endif

#if defined(HAVE_DISKARBITRATION_DISKARBITRATION_H)
	DASessionRef session = DASessionCreate(kCFAllocatorDefault);
	DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, path.c_str());
	if (disk)
	{
		CFDictionaryRef diskInfo = DADiskCopyDescription(disk);
		if (diskInfo)
		{
			// Clear existing values
			strMakeModel = strFirmwareRevision = strSerialNumber = "";

			auto strVendor = static_cast<CFStringRef>(CFDictionaryGetValue(diskInfo, kDADiskDescriptionDeviceVendorKey));
			auto strModel = static_cast<CFStringRef>(CFDictionaryGetValue(diskInfo, kDADiskDescriptionDeviceModelKey));
			auto strRevision = static_cast<CFStringRef>(CFDictionaryGetValue(diskInfo, kDADiskDescriptionDeviceRevisionKey));

			if (strVendor)
			{
				std::string s = CFStringGetCStringPtr(strVendor, 0);
				strMakeModel = util::trim(s);
			}

			if (strModel)
			{
				std::string s = CFStringGetCStringPtr(strModel, 0);
				strMakeModel += util::trim(s);
			}

			if (strRevision)
			{
				std::string s = CFStringGetCStringPtr(strRevision, 0);
				strFirmwareRevision = util::trim(s);
			}

			CFRelease(diskInfo);
		}

		CFRelease(disk);
		CFRelease(session);
	}
#endif // HAVE_DISKARBITRATION_DISKARBITRATION_H

	return false;
}