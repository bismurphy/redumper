#include <fmt/format.h>
#include <regex>
#include <set>
#include <sstream>
#include "common.hh"
#include "endian.hh"
#include "file_io.hh"
#include "hex_bin.hh"
#include "redumper.hh"
#include "psx.hh"



namespace gpsxre
{


const std::string SystemPSX::_EXE_MAGIC("PS-X EXE");

const std::vector<uint32_t> SystemPSX::_LIBCRYPT_SECTORS_BASE =
{
	13955, 14081, 14335, 14429, 14499, 14749, 14906, 14980,
	15092, 15162, 15228, 15478, 15769, 15881, 15951, 16017,
	41895, 42016, 42282, 42430, 42521, 42663, 42862, 43027,
	43139, 43204, 43258, 43484, 43813, 43904, 44009, 44162
};

const uint32_t SystemPSX::_LIBCRYPT_SECTORS_SHIFT = 5;

const std::set<uint32_t> SystemPSX::_LIBCRYPT_SECTORS_COUNT =
{
	16,
	32
};


SystemPSX::SystemPSX(const std::filesystem::path &track_path)
	: _trackPath(track_path)
	, _trackSize(std::filesystem::file_size(track_path))
{
	;
}


void SystemPSX::operator()(std::ostream &os) const
{
	if(ImageBrowser::IsDataTrack(_trackPath))
	{
		ImageBrowser browser(_trackPath, 0, _trackSize, false);

		auto exe_path = findEXE(browser);
		if(!exe_path.empty())
		{
			auto exe_file = browser.RootDirectory()->SubEntry(exe_path);
			auto exe = exe_file->Read();
			if(exe.size() >= _EXE_MAGIC.length() && std::string((char *)exe.data(), _EXE_MAGIC.length()) == _EXE_MAGIC)
			{
				os << fmt::format("PSX [{}]:", _trackPath.filename().string()) << std::endl;
				os << fmt::format("  EXE: {}", exe_path) << std::endl;

				{
					time_t t = exe_file->DateTime();
					std::stringstream ss;
					ss << std::put_time(localtime(&t), "%Y-%m-%d");
					os << fmt::format("  EXE date: {}", ss.str()) << std::endl;
				}

				auto serial = deduceSerial(exe_path);
				if(!serial.first.empty() && !serial.second.empty())
					os << fmt::format("  serial: {}-{}", serial.first, serial.second) << std::endl;

				auto region = detectRegion(serial.first);
				if(!region.empty())
					os << fmt::format("  region: {}", region) << std::endl;

				bool edc = detectEdcFast();
				os << fmt::format("  EDC: {}", edc ? "yes" : "no") << std::endl;

				{
					std::stringstream ss;
					bool antimod = findAntiModchipStrings(ss, browser);
					os << fmt::format("  anti-modchip: {}", antimod ? "yes" : "no") << std::endl;
					if(antimod)
						os << ss.str() << std::endl;
				}

				std::filesystem::path sub_path = track_extract_basename(_trackPath.string()) + ".subcode";
				if(std::filesystem::exists(sub_path))
				{
					std::stringstream ss;
					bool libcrypt = detectLibCrypt(ss, sub_path);
					os << fmt::format("  libcrypt: {}", libcrypt ? "yes" : "no") << std::endl;
					if(libcrypt)
						os << ss.str() << std::endl;
				}
			}
		}
	}
}


std::string SystemPSX::findEXE(ImageBrowser &browser) const
{
	std::string exe_path;

	auto system_cnf = browser.RootDirectory()->SubEntry("SYSTEM.CNF");
	if(system_cnf)
	{
		auto data = system_cnf->Read();
		std::string data_str(data.begin(), data.end());
		std::stringstream ss(data_str);

		std::string line;
		while(std::getline(ss, line))
		{
			// examples:
            // BOOT = cdrom:\\SCUS_945.03;1\r"   // 1Xtreme (USA)
            // BOOT=cdrom:\\SCUS_944.23;1"       // Ape Escape (USA)
            // BOOT=cdrom:\\SLPS_004.35\r"       // Megatudo 2096 (Japan)
            // BOOT = cdrom:\SLPM803.96;1"       // Chouzetsu Daigirin '99-nen Natsu-ban (Japan)
            // BOOT = cdrom:\EXE\PCPX_961.61;1   // Wild Arms - 2nd Ignition (Japan) (Demo)

			std::smatch matches;
			std::regex_match(line, matches, std::regex("^\\s*BOOT.*=\\s*cdrom.?:\\\\*(.*?)(?:;.*\\s*|\\s*$)"));
			if(matches.size() == 2)
			{
				exe_path = str_uppercase(matches.str(1));
				break;
			}
		}
	}
	else
	{
		auto psx_exe = browser.RootDirectory()->SubEntry("PSX.EXE");
		if(psx_exe)
			exe_path = psx_exe->Name();
	}

	return exe_path;
}


std::pair<std::string, std::string> SystemPSX::deduceSerial(std::string exe_path) const
{
    std::pair<std::string, std::string> serial;

    std::smatch matches;
    std::regex_match(exe_path, matches, std::regex("(.*\\\\)*([A-Z]*)(_|-)?([A-Z]?[0-9]+)\\.([0-9]+[A-Z]?)"));
    if(matches.size() == 6)
    {
        serial.first = matches.str(2);
        serial.second = matches.str(4) + matches.str(5);

        // Road Writer (USA)
        if(serial.first.empty() && serial.second == "907127001")
            serial.first = "LSP";
        // GameGenius Ver. 5.0 (Taiwan) (En,Zh) (Unl)
        else if(serial.first == "PAR" && serial.second == "90001")
        {
            serial.first.clear();
            serial.second.clear();
        }
    }

    return serial;
}


std::string SystemPSX::detectRegion(std::string prefix) const
{
    std::string region;

    const std::set<std::string> REGION_J {"ESPM", "PAPX", "PCPX", "PDPX", "SCPM", "SCPS", "SCZS", "SIPS", "SLKA", "SLPM", "SLPS"};
    const std::set<std::string> REGION_U {"LSP", "PEPX", "SCUS", "SLUS", "SLUSP"};
    const std::set<std::string> REGION_E {"PUPX", "SCED", "SCES", "SLED", "SLES"};
    // multi: "DTL", "PBPX"

    if(REGION_J.find(prefix) != REGION_J.end())
        region = "Japan";
    else if(REGION_U.find(prefix) != REGION_U.end())
        region = "USA";
    else if(REGION_E.find(prefix) != REGION_E.end())
        region = "Europe";

    return region;
}


bool SystemPSX::findAntiModchipStrings(std::ostream &os, ImageBrowser &browser) const
{
    std::vector<std::string> entries;

	// taken from DIC
	const char ANTIMOD_MESSAGE_EN[] = "     SOFTWARE TERMINATED\nCONSOLE MAY HAVE BEEN MODIFIED\n     CALL 1-888-780-7690";
	// string is encoded with Shift JIS
	const uint8_t ANTIMOD_MESSAGE_JP[] =
	{
        // 強制終了しました。
		0x8b, 0xad, 0x90, 0xa7, 0x8f, 0x49, 0x97, 0xb9, 0x82, 0xb5, 0x82, 0xdc, 0x82, 0xb5, 0x82, 0xbd, 0x81, 0x42, 0x0a,
        // 本体が改造されている
		0x96, 0x7b, 0x91, 0xcc, 0x82, 0xaa, 0x89, 0xfc, 0x91, 0xa2, 0x82, 0xb3, 0x82, 0xea, 0x82, 0xc4, 0x82, 0xa2, 0x82, 0xe9, 0x0a,
        // おそれがあります。
		0x82, 0xa8, 0x82, 0xbb, 0x82, 0xea, 0x82, 0xaa, 0x82, 0xa0, 0x82, 0xe8, 0x82, 0xdc, 0x82, 0xb7, 0x81, 0x42
	};

	browser.Iterate([&](const std::string &path, std::shared_ptr<ImageBrowser::Entry> d)
	{
		bool exit = false;

		auto fp((path.empty() ? "" : path + "/") + d->Name());

		if(!d->IsDummy() && !d->IsInterleaved())
		{
			auto data = d->Read(false, false);

			auto it_en = search(data.begin(), data.end(), std::begin(ANTIMOD_MESSAGE_EN), std::end(ANTIMOD_MESSAGE_EN));
            if(it_en != data.end())
            {
                std::stringstream ss;
                ss << fp << " @ 0x" << std::hex << it_en - data.begin() << ": EN";
                entries.emplace_back(ss.str());
            }
			auto it_jp = search(data.begin(), data.end(), std::begin(ANTIMOD_MESSAGE_JP), std::end(ANTIMOD_MESSAGE_JP));
            if(it_jp != data.end())
            {
                std::stringstream ss;
                ss << fp << " @ 0x" << std::hex << it_jp - data.begin() << ": JP";
                entries.emplace_back(ss.str());
            }
		}

		return exit;
	});

	for(auto const &s : entries)
		os << s << std::endl;

    return !entries.empty();
}


bool SystemPSX::detectEdcFast() const
{
	bool edc = false;

	std::fstream fs(_trackPath, std::fstream::in | std::fstream::binary);
	if(!fs.is_open())
		throw_line(fmt::format("unable to open file ({})", _trackPath.filename().string()));

	uint32_t sectors_count = _trackSize / CD_DATA_SIZE;
	if(sectors_count >= iso9660::SYSTEM_AREA_SIZE)
	{
		Sector sector;
		read_entry(fs, (uint8_t *)&sector, CD_DATA_SIZE, iso9660::SYSTEM_AREA_SIZE - 1, 1, 0, 0);

		if(sector.header.mode == 2 && sector.mode2.xa.sub_header.submode & (uint8_t)CDXAMode::FORM2)
			edc = sector.mode2.xa.form2.edc;
	}

	return edc;
}


bool SystemPSX::detectLibCrypt(std::ostream &os, std::filesystem::path sub_path) const
{
	bool libcrypt = false;

	std::fstream fs(sub_path, std::fstream::in | std::fstream::binary);
	if(!fs.is_open())
		throw_line(fmt::format("unable to open file ({})", sub_path.filename().string()));

	std::vector<int32_t> candidates;

	std::vector<uint8_t> sub_buffer(CD_SUBCODE_SIZE);
	int32_t lba_end = _trackSize / CD_DATA_SIZE;
	for(uint32_t i = 0; i < _LIBCRYPT_SECTORS_BASE.size(); ++i)
	{
		int32_t lba1 = _LIBCRYPT_SECTORS_BASE[i];
		int32_t lba2 = _LIBCRYPT_SECTORS_BASE[i] + _LIBCRYPT_SECTORS_SHIFT;

		if(lba1 >= lba_end || lba2 >= lba_end)
			continue;

		ChannelQ Q1;
		read_entry(fs, sub_buffer.data(), (uint32_t)sub_buffer.size(), lba1 - LBA_START, 1, 0, 0);
		subcode_extract_channel((uint8_t *)&Q1, sub_buffer.data(), Subchannel::Q);

		ChannelQ Q2;
		read_entry(fs, sub_buffer.data(), (uint32_t)sub_buffer.size(), lba2 - LBA_START, 1, 0, 0);
		subcode_extract_channel((uint8_t *)&Q2, sub_buffer.data(), Subchannel::Q);

		if(!Q1.Valid() && !Q2.Valid())
		{
			candidates.push_back(lba1);
			candidates.push_back(lba2);
		}
	}

	if(_LIBCRYPT_SECTORS_COUNT.find(candidates.size()) != _LIBCRYPT_SECTORS_COUNT.end())
	{
		for(auto const &c : candidates)
		{
			ChannelQ Q;
			read_entry(fs, sub_buffer.data(), (uint32_t)sub_buffer.size(), c - LBA_START, 1, 0, 0);
			subcode_extract_channel((uint8_t *)&Q, sub_buffer.data(), Subchannel::Q);
			MSF msf = LBA_to_MSF(c);
			os << fmt::format("MSF: {:02}:{:02}:{:02} Q-Data: {:02X}{:02X}{:02X} {:02X}:{:02X}:{:02X} {:02X} {:02X}:{:02X}:{:02X} {:04X}",
					msf.m, msf.s, msf.f, Q.control_adr, Q.mode1.tno, Q.mode1.index, Q.mode1.msf.m, Q.mode1.msf.s, Q.mode1.msf.f, Q.mode1.zero, Q.mode1.a_msf.m, Q.mode1.a_msf.s, Q.mode1.a_msf.f, endian_swap<uint16_t>(Q.crc)) << std::endl;
		}

		libcrypt = true;
	}

	return libcrypt;
}

}
