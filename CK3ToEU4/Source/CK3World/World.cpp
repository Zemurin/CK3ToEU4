#include "World.h"
#include "../Configuration/Configuration.h"
#include "../Helpers/rakaly.h"
#include "../commonItems/ParserHelpers.h"
#include "Characters/Character.h"
#include "Characters/CharacterDomain.h"
#include "CommonFunctions.h"
#include "CommonRegexes.h"
#include "Geography/CountyDetail.h"
#include "Log.h"
#include "ModLoader/ModFilesystem.h"
#include "OSCompatibilityLayer.h"
#include "Religions/Faith.h"
#include "Religions/Religion.h"
#include "Titles/Title.h"
#include <filesystem>
#include <fstream>
#include <ranges>
namespace fs = std::filesystem;

CK3::World::World(const std::shared_ptr<Configuration>& theConfiguration, const commonItems::ConverterVersion& converterVersion)
{
	registerKeys(theConfiguration, converterVersion);
	Log(LogLevel::Progress) << "4 %";

	Log(LogLevel::Info) << "-> Verifying CK3 save.";
	verifySave(theConfiguration->getSaveGamePath());
	processSave(theConfiguration->getSaveGamePath());
	Log(LogLevel::Progress) << "5 %";

	auto metaData = std::istringstream(saveGame.metadata);
	parseStream(metaData);
	Log(LogLevel::Progress) << "10 %";

	Log(LogLevel::Info) << "* Priming Converter Components *";
	primeLaFabricaDeColor(*theConfiguration);
	loadLandedTitles(*theConfiguration);
	loadCharacterTraits(*theConfiguration);
	loadHouseNames(*theConfiguration);
	Log(LogLevel::Progress) << "15 %";
	// Scraping localizations from CK3 so we may know proper names for our countries and people.
	Log(LogLevel::Info) << "-> Reading Words";
	localizationMapper.scrapeLocalizations(*theConfiguration, mods);
	cultureMapper.loadCulturesFromDisk();

	Log(LogLevel::Info) << "* Parsing Gamestate *";
	auto gameState = std::istringstream(saveGame.gamestate);
	parseStream(gameState);
	Log(LogLevel::Progress) << "20 %";
	clearRegisteredKeywords();

	Log(LogLevel::Info) << "* Gamestate Parsing Complete, Weaving Internals *";
	crosslinkDatabases();
	Log(LogLevel::Progress) << "30 %";

	// processing
	Log(LogLevel::Info) << "-- Checking For Religions";
	checkForIslam();
	Log(LogLevel::Info) << "-- Flagging HRE Provinces";
	flagHREProvinces(*theConfiguration);
	Log(LogLevel::Info) << "-- Shattering HRE";
	shatterHRE(*theConfiguration);
	Log(LogLevel::Info) << "-- Shattering Empires";
	shatterEmpires(*theConfiguration);
	Log(LogLevel::Info) << "-- Filtering Independent Titles";
	filterIndependentTitles();
	Log(LogLevel::Info) << "-- Splitting Off Vassals";
	splitVassals(*theConfiguration);
	Log(LogLevel::Info) << "-- Rounding Up Some People";
	gatherCourtierNames();
	Log(LogLevel::Info) << "-- Congregating DeFacto Counties for Independent Titles";
	congregateDFCounties();
	Log(LogLevel::Info) << "-- Congregating DeJure Counties for Independent Titles";
	congregateDJCounties();
	Log(LogLevel::Info) << "-- Filtering Landless Titles";
	filterLandlessTitles();
	Log(LogLevel::Info) << "-- Distributing Electorates";
	setElectors();

	if (coaDesigner && playerID)
	{
		Log(LogLevel::Info) << "-- Locating Player Title.";
		locatePlayerTitle(theConfiguration);
	}

	Log(LogLevel::Info) << "*** Good-bye CK3, rest in peace. ***";
	Log(LogLevel::Progress) << "47 %";
}

void CK3::World::registerKeys(const std::shared_ptr<Configuration>& theConfiguration, const commonItems::ConverterVersion& converterVersion)
{
	Log(LogLevel::Info) << "*** Hello CK3, Deus Vult! ***";
	metaPreParser.registerRegex("SAV.*", [](const std::string& unused, std::istream& theStream) {
	});
	metaPreParser.registerKeyword("meta_data", [this](std::istream& theStream) {
		metaParser.parseStream(theStream);
		saveGame.parsedMeta = true;
	});
	metaPreParser.registerRegex(commonItems::catchallRegex, commonItems::ignoreItem);

	metaParser.registerKeyword("mods", [this, theConfiguration](std::istream& theStream) {
		Log(LogLevel::Info) << "-> Detecting used mods.";
		std::set<std::string> seenMods;
		for (const auto& path: commonItems::getStrings(theStream))
		{
			if (seenMods.contains(path))
				continue;
			mods.emplace_back(Mod("", path));
			seenMods.emplace(path);
		}
		Log(LogLevel::Info) << "<> Savegame claims " << mods.size() << " mods used.";
		commonItems::ModLoader modLoader;
		modLoader.loadMods(theConfiguration->getCK3DocPath(), mods);
		mods = modLoader.getMods();
		for (const auto& mod: mods)
			if (mod.name == "CoA Designer")
			{
				Log(LogLevel::Notice) << "CoA Designer mod enabed; player CoA will be generated.";
				coaDesigner = true;
			}
	});
	metaParser.registerRegex(commonItems::catchallRegex, commonItems::ignoreItem);

	registerRegex("SAV.*", [](const std::string& unused, std::istream& theStream) {
	});
	registerKeyword("meta_data", [this](std::istream& theStream) {
		if (saveGame.parsedMeta)
		{
			commonItems::ignoreItem("unused", theStream);
		}
		else
		{
			metaParser.parseStream(theStream);
			saveGame.parsedMeta = true;
		}
	});
	registerKeyword("currently_played_characters", [this](std::istream& theStream) {
		auto playedCharacters = commonItems::getLlongs(theStream);
		if (!playedCharacters.empty())
			playerID = playedCharacters[0];
	});
	registerKeyword("date", [this](const std::string& unused, std::istream& theStream) {
		const commonItems::singleString dateString(theStream);
		endDate = date(dateString.getString());
	});
	registerKeyword("bookmark_date", [this](const std::string& unused, std::istream& theStream) {
		const commonItems::singleString startDateString(theStream);
		startDate = date(startDateString.getString());
	});
	registerKeyword("version", [this, converterVersion](const std::string& unused, std::istream& theStream) {
		const commonItems::singleString versionString(theStream);
		CK3Version = GameVersion(versionString.getString());
		Log(LogLevel::Info) << "<> Savegame version: " << versionString.getString();

		if (converterVersion.getMinSource() > CK3Version)
		{
			Log(LogLevel::Error) << "Converter requires a minimum save from v" << converterVersion.getMinSource().toShortString();
			throw std::runtime_error("Savegame vs converter version mismatch!");
		}
		if (!converterVersion.getMaxSource().isLargerishThan(CK3Version))
		{
			Log(LogLevel::Error) << "Converter requires a maximum save from v" << converterVersion.getMaxSource().toShortString();
			throw std::runtime_error("Savegame vs converter version mismatch!");
		}
	});
	registerKeyword("variables", [this](const std::string& unused, std::istream& theStream) {
		Log(LogLevel::Info) << "-> Loading variable flags.";
		flags = Flags(theStream);
		Log(LogLevel::Info) << "<> Loaded " << flags.getFlags().size() << " variable flags.";
	});
	registerKeyword("landed_titles", [this](const std::string& unused, std::istream& theStream) {
		Log(LogLevel::Info) << "-> Loading titles.";
		titles = Titles(theStream);
		const auto& counter = titles.getCounter();
		Log(LogLevel::Info) << "<> Loaded " << titles.getTitles().size() << " titles: " << counter[0] << "b " << counter[1] << "c " << counter[2] << "d "
								  << counter[3] << "k " << counter[4] << "e, " << counter[5] << "dynamics.";
	});
	registerKeyword("provinces", [this](const std::string& unused, std::istream& theStream) {
		Log(LogLevel::Info) << "-> Loading provinces.";
		provinceHoldings = ProvinceHoldings(theStream);
		Log(LogLevel::Info) << "<> Loaded " << provinceHoldings.getProvinceHoldings().size() << " provinces.";
	});
	registerKeyword("living", [this](std::istream& theStream) {
		Log(LogLevel::Info) << "-> Loading potentially alive human beings.";
		characters.loadCharacters(theStream);
		Log(LogLevel::Info) << "<> Loaded " << characters.getCharacters().size() << " human entities.";
	});
	registerKeyword("dead_unprunable", [this](const std::string& unused, std::istream& theStream) {
		Log(LogLevel::Info) << "-> Loading dead people.";
		characters.loadCharacters(theStream);
		Log(LogLevel::Info) << "<> Loaded " << characters.getCharacters().size() << " human remains.";
	});
	registerKeyword("dynasties", [this](const std::string& unused, std::istream& theStream) {
		Log(LogLevel::Info) << "-> Loading dynasties.";
		dynasties = Dynasties(theStream);
		houses = dynasties.getHouses(); // Do not access houses in dynasties after this - there are none and will crash.
		Log(LogLevel::Info) << "<> Loaded " << dynasties.getDynasties().size() << " dynasties and " << houses.getHouses().size() << " houses.";
	});
	registerKeyword("religion", [this](const std::string& unused, std::istream& theStream) {
		Log(LogLevel::Info) << "-> Loading religions.";
		religions = Religions(theStream);
		faiths = religions.getFaiths(); // Do not access faiths in religions after this - there are none and will crash.
		Log(LogLevel::Info) << "<> Loaded " << religions.getReligions().size() << " religions and " << faiths.getFaiths().size() << " faiths.";
	});
	registerKeyword("coat_of_arms", [this](const std::string& unused, std::istream& theStream) {
		Log(LogLevel::Info) << "-> Loading garments of limbs.";
		coats = CoatsOfArms(theStream);
		Log(LogLevel::Info) << "<> Loaded " << coats.getCoats().size() << " wearables.";
	});
	registerKeyword("county_manager", [this](const std::string& unused, std::istream& theStream) {
		Log(LogLevel::Info) << "-> Loading county details.";
		countyDetails = CountyDetails(theStream);
		Log(LogLevel::Info) << "<> Loaded " << countyDetails.getCountyDetails().size() << " county details.";
	});
	registerKeyword("culture_manager", [this](const std::string& unused, std::istream& theStream) {
		Log(LogLevel::Info) << "-> Loading cultures.";
		cultures = Cultures(theStream);
		Log(LogLevel::Info) << "<> Loaded " << cultures.getCultures().size() << " cultures.";
	});
	registerKeyword("confederation_manager", [this](const std::string& unused, std::istream& theStream) {
		Log(LogLevel::Info) << "-> Loading confederations.";
		confederations = Confederations(theStream);
		Log(LogLevel::Info) << "<> Loaded " << confederations.getConfederations().size() << " confederations.";
	});
	registerRegex(commonItems::catchallRegex, commonItems::ignoreItem);
}

void CK3::World::locatePlayerTitle(const std::shared_ptr<Configuration>& theConfiguration)
{
	for (const auto& title: independentTitles)
		if (title.second->getHolder() && title.second->getHolder()->first == *playerID)
		{
			Log(LogLevel::Info) << "Player title: " << title.second->getName();
			playerTitle = title.first;
			break;
		}

	if (playerTitle)
	{
		theConfiguration->setCraftFlagForPlayerTitle(*playerTitle);
	}
}

void CK3::World::processSave(const std::filesystem::path& saveGamePath)
{
	std::ifstream saveFile(saveGamePath, std::ios::binary);
	std::stringstream inStream;
	inStream << saveFile.rdbuf();
	saveGame.gamestate = inStream.str();

	const auto save = rakaly::parseCk3(saveGame.gamestate);
	if (const auto& melt = save.meltMeta(); melt)
	{
		Log(LogLevel::Info) << "Meta extracted successfully.";
		melt->writeData(saveGame.metadata);
	}
	else if (save.is_binary())
	{
		Log(LogLevel::Error) << "Binary Save and NO META!";
	}

	if (save.is_binary())
	{
		Log(LogLevel::Info) << "Gamestate is binary, melting.";
		const auto& melt = save.melt();
		if (melt.has_unknown_tokens())
		{
			Log(LogLevel::Error) << "Rakaly reports errors while melting ironman save!";
		}

		melt.writeData(saveGame.gamestate);
	}
	else
	{
		Log(LogLevel::Info) << "Gamestate is textual.";
		const auto& melt = save.melt();
		melt.writeData(saveGame.gamestate);
	}

	// Always dump to disk for easier debug.
	std::ofstream metaDump("metaDump.txt");
	metaDump << saveGame.metadata;
	metaDump.close();

	std::ofstream saveDump("saveDump.txt");
	saveDump << saveGame.gamestate;
	saveDump.close();
}

void CK3::World::verifySave(const std::filesystem::path& saveGamePath) const
{
	std::ifstream saveFile(saveGamePath, std::ios::binary);
	if (!saveFile.is_open())
		throw std::runtime_error("Could not open save! Exiting!");

	char buffer[10];
	saveFile.get(buffer, 4);
	if (buffer[0] != 'S' || buffer[1] != 'A' || buffer[2] != 'V')
		throw std::runtime_error("Savefile of unknown type.");

	saveFile.close();
}

void CK3::World::primeLaFabricaDeColor(const Configuration& theConfiguration)
{
	Log(LogLevel::Info) << "-> Loading colors.";
	for (const auto& file: commonItems::GetAllFilesInFolder(theConfiguration.getCK3Path() / "common/named_colors"))
	{
		if (file.extension() != ".txt")
			continue;
		namedColors.loadColors(theConfiguration.getCK3Path() / "common/named_colors" / file);
	}
	for (const auto& mod: mods)
	{
		if (!commonItems::DoesFolderExist(mod.path / "common/named_colors"))
			continue;
		Log(LogLevel::Info) << "<> Loading some colors from [" << mod.name << "]";
		for (const auto& file: commonItems::GetAllFilesInFolder(mod.path / "common/named_colors"))
		{
			if (file.extension() != ".txt")
				continue;
			namedColors.loadColors(mod.path / "common/named_colors" / file);
		}
	}
	Log(LogLevel::Info) << "<> Loaded " << laFabricaDeColor.getRegisteredColors().size() << " colors.";
}

void CK3::World::loadLandedTitles(const Configuration& theConfiguration)
{
	Log(LogLevel::Info) << "-> Loading Landed Titles.";
	commonItems::ModFilesystem modFS(theConfiguration.getCK3Path(), mods);
	for (const auto& file: modFS.GetAllFilesInFolder("common/landed_titles"))
	{
		if (file.extension() != ".txt")
			continue;
		landedTitles.loadTitles(file);
	}
	Log(LogLevel::Info) << "<> Loaded " << landedTitles.getFoundTitles().size() << " landed titles.";
}

void CK3::World::loadCharacterTraits(const Configuration& theConfiguration)
{
	Log(LogLevel::Info) << "-> Examiming Personalities";
	for (const auto& file: commonItems::GetAllFilesInFolder(theConfiguration.getCK3Path() / "common/traits"))
	{
		if (file.extension() != ".txt")
			continue;
		traitScraper.loadTraits(theConfiguration.getCK3Path() / "common/traits" / file);
	}
	for (const auto& mod: mods)
	{
		if (!commonItems::DoesFolderExist(mod.path / "common/traits"))
			continue;
		Log(LogLevel::Info) << "<> Loading some character traits from [" << mod.name << "]";
		for (const auto& file: commonItems::GetAllFilesInFolder(mod.path / "common/traits"))
		{
			if (file.extension() != ".txt")
				continue;
			traitScraper.loadTraits(mod.path / "common/traits" / file);
		}
	}
	Log(LogLevel::Info) << ">> " << traitScraper.getTraits().size() << " personalities scrutinized.";
}

void CK3::World::loadHouseNames(const Configuration& theConfiguration)
{
	Log(LogLevel::Info) << "-> Loading House Names";
	for (const auto& file: commonItems::GetAllFilesInFolder(theConfiguration.getCK3Path() / "common/dynasty_houses"))
	{
		if (file.extension() != ".txt")
			continue;
		houseNameScraper.loadHouseDetails(theConfiguration.getCK3Path() / "common/dynasty_houses" / file);
	}
	for (const auto& mod: mods)
	{
		if (!commonItems::DoesFolderExist(mod.path / "common/dynasty_houses"))
			continue;
		Log(LogLevel::Info) << "<> Loading house names from [" << mod.name << "]";
		for (const auto& file: commonItems::GetAllFilesInFolder(mod.path / "common/dynasty_houses"))
		{
			if (file.extension() != ".txt")
				continue;
			houseNameScraper.loadHouseDetails(mod.path / "common/dynasty_houses" / file);
		}
	}
	Log(LogLevel::Info) << ">> " << houseNameScraper.getHouseNames().size() << " house names read.";
}

void CK3::World::crosslinkDatabases()
{
	Log(LogLevel::Info) << "-> Injecting Names into Houses.";
	houses.importNames(houseNameScraper);

	Log(LogLevel::Info) << "-> Concocting Cultures.";
	cultures.concoctCultures(localizationMapper, cultureMapper);

	std::set<std::shared_ptr<Culture>> cultureSet;
	for (const auto& culture: cultures.getCultures() | std::views::values)
		cultureSet.insert(culture);
	cultureMapper.storeCultures(cultureSet);
	Log(LogLevel::Info) << "-> Loading Cultures into Counties.";
	countyDetails.linkCultures(cultures);
	Log(LogLevel::Info) << "-> Loading Cultures into Characters.";
	characters.linkCultures(cultures);
	Log(LogLevel::Info) << "-> Loading Faiths into Counties.";
	countyDetails.linkFaiths(faiths);
	Log(LogLevel::Info) << "-> Loading Faiths into Characters.";
	characters.linkFaiths(faiths);
	Log(LogLevel::Info) << "-> Loading Faiths into Religions.";
	religions.linkFaiths(faiths);
	Log(LogLevel::Info) << "-> Loading Religions into Faiths.";
	faiths.linkReligions(religions, titles);
	Log(LogLevel::Info) << "-> Loading Titles into Coats.";
	coats.linkParents(titles);
	Log(LogLevel::Info) << "-> Loading Coats into Dynasties.";
	dynasties.linkCoats(coats);
	Log(LogLevel::Info) << "-> Loading Coats into Titles.";
	titles.linkCoats(coats);
	Log(LogLevel::Info) << "-> Loading Holdings into Clay.";
	landedTitles.linkProvinceHoldings(provinceHoldings);
	Log(LogLevel::Info) << "-> Loading Counties into Clay.";
	landedTitles.linkCountyDetails(countyDetails);
	Log(LogLevel::Info) << "-> Loading Dynasties into Houses.";
	houses.linkDynasties(dynasties);
	Log(LogLevel::Info) << "-> Loading Characters into Houses.";
	houses.linkCharacters(characters);
	Log(LogLevel::Info) << "-> Loading Houses into Characters.";
	characters.linkHouses(houses);
	Log(LogLevel::Info) << "-> Loading Characters into Titles.";
	titles.linkCharacters(characters);
	Log(LogLevel::Info) << "-> Loading Titles into Characters.";
	characters.linkTitles(titles);
	Log(LogLevel::Info) << "-> Loading Titles into Titles.";
	titles.linkTitles();
	Log(LogLevel::Info) << "-> Fixing Titles Pointing To Wrong Places.";
	titles.relinkDeFactoVassals();
	Log(LogLevel::Info) << "-> Loading Titles into Clay.";
	landedTitles.linkTitles(titles);
	Log(LogLevel::Info) << "-> Loading Characters into Characters.";
	characters.linkCharacters();
	Log(LogLevel::Info) << "-> Loading Clay into Titles.";
	titles.linkLandedTitles(landedTitles);
	Log(LogLevel::Info) << "-> Loading Traits into Characters.";
	characters.linkTraits(traitScraper);
	Log(LogLevel::Info) << "-> Loading Characters into Confederations.";
	confederations.linkCharacters(characters);
	Log(LogLevel::Info) << "-> Loading Coats into Confederations.";
	confederations.linkCoats(coats);
}

void CK3::World::flagHREProvinces(const Configuration& theConfiguration)
{
	std::string hreTitleStr;
	switch (theConfiguration.getHRE())
	{
		case Configuration::I_AM_HRE::HRE:
			hreTitleStr = "e_hre";
			break;
		case Configuration::I_AM_HRE::BYZANTIUM:
			hreTitleStr = "e_byzantium";
			break;
		case Configuration::I_AM_HRE::ROME:
			hreTitleStr = "e_roman_empire";
			break;
		case Configuration::I_AM_HRE::CUSTOM:
			hreTitleStr = iAmHreMapper.getHRE();
			break;
		case Configuration::I_AM_HRE::NONE:
			Log(LogLevel::Info) << ">< HRE Provinces not available due to configuration disabling HRE Mechanics.";
			return;
	}
	const auto& allTitles = titles.getTitles();
	const auto& theHre = allTitles.find(hreTitleStr);
	if (theHre == allTitles.end())
	{
		Log(LogLevel::Info) << ">< HRE Provinces not available, " << hreTitleStr << " not found!";
		return;
	}
	if (theHre->second->getDFVassals().empty())
	{
		Log(LogLevel::Info) << ">< HRE Provinces not available, " << hreTitleStr << " has no vassals!";
		return;
	}
	if (!theHre->second->getHolder())
	{
		Log(LogLevel::Info) << ">< HRE Provinces not available, " << hreTitleStr << " has no holder!";
		return;
	}

	// store for later.
	hreTitle = std::make_pair(hreTitleStr, theHre->second);

	const auto counter = theHre->second->flagDeJureHREProvinces();
	Log(LogLevel::Info) << "<> " << counter << " HRE provinces flagged.";
}

void CK3::World::checkForIslam()
{
	for (const auto& county: countyDetails.getCountyDetails() | std::views::values)
	{
		if (!county->getFaith().second)
			continue;
		if (!county->getFaith().second->getReligion().second)
			continue;
		if (county->getFaith().second->getReligion().second->getName() == "islam_religion")
		{
			islamExists = true;
			return;
		}
	}
}

void CK3::World::shatterHRE(const Configuration& theConfiguration) const
{
	if (!hreTitle)
		return;
	const auto& hreHolder = hreTitle->second->getHolder();
	Log(LogLevel::Info) << "HRE Holder: " << hreHolder->second->getName();
	bool emperorSet = false; // "Emperor", in this context, is not a person but the resulting primary duchy/kingdom title of said person.
	std::map<long long, std::shared_ptr<Character>> brickedPeople; // these are people we need to fix.

	// First we are composing a list of all HRE members. These are duchies,
	// so we're also ripping them from under any potential kingdoms.
	std::map<long long, std::shared_ptr<Title>> hreMembers;
	std::map<long long, std::shared_ptr<Title>> brickList;
	for (const auto& vassal: hreTitle->second->getDFVassals())
	{
		if (vassal.second->getLevel() == LEVEL::DUCHY || vassal.second->getLevel() == LEVEL::COUNTY)
		{
			hreMembers.insert(vassal);
		}
		else if (vassal.second->getLevel() == LEVEL::KINGDOM)
		{
			if (vassal.second->getName() == "k_papal_state" || vassal.second->getName() == "k_orthodox" ||
				 theConfiguration.getShatterHRELevel() == Configuration::SHATTER_HRE_LEVEL::KINGDOM) // hard override for special HRE members
			{
				hreMembers.insert(vassal);
			}
			else
			{
				for (const auto& vassalvassal: vassal.second->getDFVassals())
				{
					hreMembers.insert(vassalvassal);
				}
				// Bricking the kingdom.
				brickedPeople.insert(*vassal.second->getHolder());
				brickList.insert(vassal);
			}
		}
		else if (vassal.second->getLevel() != LEVEL::BARONY)
		{
			Log(LogLevel::Warning) << "Unrecognized HRE vassal: " << vassal.first << " - " << vassal.second->getName();
		}
	}

	for (const auto& brick: brickList)
		brick.second->brickTitle();

	// Locating HRE emperor. Unlike CK2, we'll using first non-hreTitle non-landless title from hreHolder's domain.
	if (!hreHolder->second->getCharacterDomain())
		throw std::runtime_error("HREmperor has no Character Domain!");

	for (const auto& hreHolderTitle: hreHolder->second->getCharacterDomain()->getDomain())
	{
		if (hreHolderTitle.second->getName() == hreTitle->first) // this is what we're breaking, ignore it.
			continue;
		if (hreHolderTitle.second->getLevel() == LEVEL::BARONY) // Absolutely ignore baronies.
			continue;
		if (hreHolderTitle.second->getLevel() == LEVEL::KINGDOM && theConfiguration.getShatterHRELevel() == Configuration::SHATTER_HRE_LEVEL::DUTCHY)
			continue; // This is bricked.
		if (hreHolderTitle.second->getClay() && !hreHolderTitle.second->getClay()->isLandless())
		{ // looks solid.
			hreHolderTitle.second->setHREEmperor();
			Log(LogLevel::Info) << "Flagging " << hreHolderTitle.second->getName() << " as His HREship.";
			emperorSet = true;
			break;
		}
	}

	if (!emperorSet)
		Log(LogLevel::Warning) << "Couldn't flag His HREship as emperor does not own any viable titles!";

	// We're flagging hre members as such, as well as setting them free.
	for (const auto& member: hreMembers)
	{
		member.second->setInHRE();
		member.second->grantIndependence(); // This fill free emperor's holdings as well. We'll reintegrate them immediately after this.
	}

	// Finally we brick the hre.
	brickedPeople.insert(*hreHolder);
	hreTitle->second->brickTitle();

	// Exception to ripping are some members that are (still) in hreHolder's domain. These would be some titles he had that were not bricked but went
	// independent; If hreHolder has empire+duchy+county, county may not go free and needs to go back under the duchy. Now that we've cleaned up bricked title(s)
	// from his domain, we can fix the loose ones.

	for (const auto& afflictedPerson: brickedPeople)
	{
		const auto& holderDomain = afflictedPerson.second->getCharacterDomain()->getDomain();
		const auto holderTitles = std::map(holderDomain.begin(), holderDomain.end());

		for (const auto& holderTitle: holderDomain)
		{
			// does this title have a DJLiege that is was in his domain, and survived bricking, but does not have DFLiege since it was granted independence?
			if (!holderTitle.second->getDFLiege() && holderTitle.second->getDJLiege() && holderTitle.second->getDJLiege()->second->getHolder() &&
				 holderTitle.second->getDJLiege()->second->getHolder()->first == afflictedPerson.first && holderTitles.count(holderTitle.first))
			{
				// fix this title.
				const auto& djLiege = holderTitle.second->getDJLiege();
				djLiege->second->addDFVassals(std::map{holderTitle});
				holderTitle.second->loadDFLiege(*djLiege);
			}
		}
	}

	Log(LogLevel::Info) << "<> " << hreMembers.size() << " HRE members released.";
}

void CK3::World::shatterEmpires(const Configuration& theConfiguration) const
{
	if (theConfiguration.getShatterEmpires() == Configuration::SHATTER_EMPIRES::NONE)
	{
		Log(LogLevel::Info) << ">< Empire shattering disabled by configuration.";
		return;
	}

	bool shatterKingdoms = true; // the default.
	switch (theConfiguration.getShatterLevel())
	{
		case Configuration::SHATTER_LEVEL::KINGDOM:
			shatterKingdoms = false;
			break;
		case Configuration::SHATTER_LEVEL::DUTCHY:
			shatterKingdoms = true;
			break;
	}
	const auto& allTitles = titles.getTitles();

	for (const auto& empire: allTitles)
	{
		if (hreTitle && empire.first == hreTitle->first)
			continue; // This is HRE, wrong function for that one.
		if (theConfiguration.getShatterEmpires() == Configuration::SHATTER_EMPIRES::CUSTOM && !shatterEmpiresMapper.isEmpireShatterable(empire.first))
			continue; // Only considering those listed.
		if (empire.second->getLevel() != LEVEL::EMPIRE && theConfiguration.getShatterEmpires() != Configuration::SHATTER_EMPIRES::CUSTOM)
			continue; // Otherwise only empires.
		if (empire.second->getDFVassals().empty())
			continue; // Not relevant.
		if (!empire.second->getHolder())
			continue; // No holder.

		std::map<long long, std::shared_ptr<Character>> brickedPeople; // these are people we need to fix.
		// First we are composing a list of all members.
		std::map<long long, std::shared_ptr<Title>> members;
		std::map<long long, std::shared_ptr<Title>> brickList;
		for (const auto& vassal: empire.second->getDFVassals())
		{
			if (!vassal.second)
			{
				Log(LogLevel::Warning) << "Shattering vassal " << vassal.first << " that isn't linked! Skipping!";
				continue;
			}
			if (vassal.second->getLevel() == LEVEL::DUCHY || vassal.second->getLevel() == LEVEL::COUNTY)
			{
				members.insert(vassal);
			}
			else if (vassal.second->getLevel() == LEVEL::KINGDOM)
			{
				if (shatterKingdoms && vassal.second->getName() != "k_papal_state" && vassal.second->getName() != "k_orthodox")
				{ // hard override for special empire members

					for (const auto& vassalVassal: vassal.second->getDFVassals())
					{
						if (!vassalVassal.second)
							Log(LogLevel::Warning) << "VassalVassal " << vassalVassal.first << " has no link!";
						else
							members.insert(vassalVassal);
					}
					// Bricking the kingdom
					if (!vassal.second->getHolder()->second)
					{
						Log(LogLevel::Warning) << "Vassal " << vassal.second->getName() << " has no holder linked!";
					}
					else
					{
						brickedPeople.insert(*vassal.second->getHolder());
					}
					brickList.insert(vassal);
				}
				else
				{
					// Not shattering kingdoms.
					members.insert(vassal);
				}
			}
			else if (vassal.second->getLevel() != LEVEL::BARONY)
			{
				Log(LogLevel::Warning) << "Unrecognized vassal level: " << vassal.first;
			}
		}

		for (const auto& brick: brickList)
			brick.second->brickTitle();

		// grant independence to ex-vassals.
		for (const auto& member: members)
		{
			member.second->grantIndependence();
		}

		// Finally, dispose of the shell.
		brickedPeople.insert(*empire.second->getHolder());
		empire.second->brickTitle();

		// Same as with HREmperor, we need to roll back counties or duchies that got released from ex-emperor himself or kings.
		for (const auto& afflictedPerson: brickedPeople)
		{
			if (!afflictedPerson.second)
			{
				Log(LogLevel::Warning) << "Character " << afflictedPerson.first << " has no link! Cannot fix them.";
				continue;
			}
			else if (!afflictedPerson.second->getCharacterDomain())
			{
				Log(LogLevel::Warning) << "Character " << afflictedPerson.first << " has no link to domain! Cannot fix them.";
				continue;
			}
			else if (afflictedPerson.second->getCharacterDomain()->getDomain().empty())
			{
				continue;
			}

			const auto& holderDomain = afflictedPerson.second->getCharacterDomain()->getDomain();
			const auto holderTitles = std::map(holderDomain.begin(), holderDomain.end());

			for (const auto& holderTitle: holderDomain)
			{
				// does this title have a DJLiege that is was in his domain, and survived bricking, but does not have DFLiege since it was granted independence?
				if (!holderTitle.second->getDFLiege() && holderTitle.second->getDJLiege() && holderTitle.second->getDJLiege()->second->getHolder() &&
					 holderTitle.second->getDJLiege()->second->getHolder()->first == afflictedPerson.first && holderTitles.count(holderTitle.first))
				{
					// fix this title.
					const auto& djLiege = holderTitle.second->getDJLiege();
					djLiege->second->addDFVassals(std::map{holderTitle});
					holderTitle.second->loadDFLiege(*djLiege);
				}
			}
		}

		Log(LogLevel::Info) << "<> " << empire.first << " shattered, " << members.size() << " members released.";
	}
}

void CK3::World::filterIndependentTitles()
{
	const auto& allTitles = titles.getTitles();
	std::map<std::string, std::shared_ptr<Title>> potentialIndeps;

	for (const auto& title: allTitles)
	{
		if (!title.second->getHolder())
			continue; // don't bother with titles without holders.
		if (!title.second->getDFLiege())
		{
			// this is a potential indep.
			potentialIndeps.insert(title);
		}
		if (title.second->getDFLiege() && !title.second->getDFLiege()->second->getHolder()) // yes, we can have a dfliege that's destroyed, apparently.
		{
			// this is also potential indep.
			potentialIndeps.insert(title);
			// And do fix it.
			title.second->grantIndependence();
		}
	}

	// Check if the holder holds any actual land (b|c_something). (Only necessary for the holder,
	// no need to recurse, we're just filtering landless titular titles like mercenaries
	// or landless Pope. If a character holds a landless titular title along actual title
	// (like Caliphate), it's not relevant at this stage as he's independent anyway.

	// First, split off all county_title holders into a container.
	std::set<long long> countyHolders;
	std::map<long long, std::map<std::string, std::shared_ptr<Title>>> allTitleHolders;
	for (const auto& title: allTitles)
	{
		if (title.second->getHolder())
		{
			if (title.second->getLevel() == LEVEL::COUNTY)
				countyHolders.insert(title.second->getHolder()->first);
			allTitleHolders[title.second->getHolder()->first].insert(title);
		}
	}

	// Then look at all potential indeps and see if their holders hold physical clay.
	auto counter = 0;
	for (const auto& indep: potentialIndeps)
	{
		const auto& holderID = indep.second->getHolder()->first;
		if (countyHolders.count(holderID))
		{
			// this fellow holds a county, so his indep title is an actual title.
			independentTitles.insert(indep);
			counter++;
			// Set The Pope
			if (indep.first == "k_papal_state")
			{
				indep.second->setThePope();
				Log(LogLevel::Info) << "---> " << indep.first << " is the Pope.";
			}
			else
			{
				if (allTitleHolders[holderID].count("k_papal_state"))
				{
					indep.second->setThePope();
					Log(LogLevel::Info) << "---> " << indep.first << " belongs to the Pope.";
				}
			}
		}
	}
	Log(LogLevel::Info) << "<> " << counter << " independent titles recognized.";
}

void CK3::World::splitVassals(const Configuration& theConfiguration)
{
	if (theConfiguration.getSplitVassals() == Configuration::SPLITVASSALS::NO)
	{
		Log(LogLevel::Info) << ">< Splitting vassals disabled by configuration.";
		return;
	}

	std::map<std::string, std::shared_ptr<Title>> newIndeps;

	// We know who's independent. We can go through all indeps and see what should be an independent vassal.
	for (const auto& title: independentTitles)
	{
		if (title.second->isThePope())
			continue; // Not touching the pope.
		// let's not split hordes or tribals. <- TODO: Add horde here once some DLC drops.
		if (title.second->getHolder()->second->getCharacterDomain()->getGovernment() == "tribal_government")
			continue;
		auto relevantVassals = 0;
		LEVEL relevantVassalLevel;
		if (title.second->getLevel() == LEVEL::EMPIRE)
			relevantVassalLevel = LEVEL::KINGDOM;
		else if (title.second->getLevel() == LEVEL::KINGDOM)
			relevantVassalLevel = LEVEL::DUCHY;
		else
			continue; // Not splitting off counties.
		for (const auto& vassal: title.second->getDFVassals())
		{
			if (vassal.second->getLevel() != relevantVassalLevel)
				continue; // they are not relevant
			if (vassal.second->coalesceDFCounties().empty())
				continue; // no land, not relevant
			relevantVassals++;
		}
		if (!relevantVassals)
			continue;																		// no need to split off anything.
		const auto& countiesClaimed = title.second->coalesceDFCounties(); // this is our primary total.
		for (const auto& vassal: title.second->getDFVassals())
		{
			if (vassal.second->getLevel() != relevantVassalLevel)
				continue; // they are not relevant
			if (vassal.second->getHolder()->first == title.second->getHolder()->first)
				continue; // Not splitting our own land.
			const auto& vassalProvincesClaimed = vassal.second->coalesceDFCounties();

			// a vassal goes indep if they control 1/relevantvassals + 10% land.
			double threshold = static_cast<double>(countiesClaimed.size()) / relevantVassals + 0.1 * static_cast<double>(countiesClaimed.size());
			threshold *= vassalSplitoffMapper.getFactor();
			if (static_cast<double>(vassalProvincesClaimed.size()) > threshold)
				newIndeps.insert(std::pair(vassal.second->getName(), vassal.second));
		}
	}

	// Now let's free them.
	for (const auto& newIndep: newIndeps)
	{
		const auto& liege = newIndep.second->getDFLiege();
		liege->second->addGeneratedVassal(newIndep);
		newIndep.second->loadGeneratedLiege(std::pair(liege->second->getName(), liege->second));
		newIndep.second->grantIndependence();
		independentTitles.insert(newIndep);
	}
	Log(LogLevel::Info) << "<> " << newIndeps.size() << " vassals liberated from immediate integration.";
}

void CK3::World::gatherCourtierNames()
{
	// We're using this function to locate courtiers, assemble their names as potential Monarch Names in EU4,
	// and also while at it, to see if they hold adviser jobs. It's anything but trivial, as being employed doesn't equate with
	// being a councilor, nor do landed councilors have employers if they work for their liege.

	auto counter = 0;
	auto counterAdvisors = 0;
	std::map<long long, std::map<std::string, bool>> holderCourtiers;								// holder-name/male
	std::map<long long, std::map<long long, std::shared_ptr<Character>>> holderCouncilors; // holder-councilors

	for (const auto& character: characters.getCharacters())
	{
		// Do you even exist?
		if (!character.second)
			continue;
		// Hello. Are you an employed individual?
		if (!character.second->isCouncilor() && !character.second->getEmployer())
			continue;
		// If you have a steady job, we need your employer's references.
		if (character.second->isCouncilor())
		{
			if (character.second->getEmployer() && character.second->getEmployer()->second)
			{
				// easiest case.
				holderCourtiers[character.second->getEmployer()->first].insert(std::pair(character.second->getName(), !character.second->isFemale()));
				holderCouncilors[character.second->getEmployer()->first].insert(character);
			}
			else if (character.second->getCharacterDomain() && !character.second->getCharacterDomain()->getDomain().empty())
			{
				// this councilor is landed and works for his liege.
				const auto& characterPrimaryTitle = character.second->getCharacterDomain()->getDomain()[0];
				if (!characterPrimaryTitle.second)
					continue; // corruption
				const auto& liegeTitle = characterPrimaryTitle.second->getDFLiege();
				if (!liegeTitle || liegeTitle->second)
					continue; // I dislike this character. I think it is time he was let go.
				const auto& liege = liegeTitle->second->getHolder();
				if (!liege || !liege->second)
					continue; // Or maybe we should fire his liege.
				holderCourtiers[liege->first].insert(std::pair(character.second->getName(), character.second->isFemale()));
				holderCouncilors[liege->first].insert(character);
			}
			else
			{
				// Doesn't have employer and doesn't have land but is councilor. Bollocks.
				continue;
			}
		}
		else if (character.second->getEmployer())
		{
			// Being employed but without a council task means a knight or physician or similar. Works for us.
			holderCourtiers[character.second->getEmployer()->first].insert(std::pair(character.second->getName(), !character.second->isFemale()));
		}
	}

	// We're only interested in those working for indeps.
	for (const auto& title: independentTitles)
	{
		const auto containerItr = holderCourtiers.find(title.second->getHolder()->first);
		if (containerItr != holderCourtiers.end())
		{
			title.second->getHolder()->second->loadCourtierNames(containerItr->second);
			counter += static_cast<int>(containerItr->second.size());
		}
		const auto councilorItr = holderCouncilors.find(title.second->getHolder()->first);
		if (councilorItr != holderCouncilors.end())
		{
			title.second->getHolder()->second->loadCouncilors(councilorItr->second);
			counterAdvisors += static_cast<int>(councilorItr->second.size());
		}
	}
	Log(LogLevel::Info) << "<> " << counter << " people gathered for interrogation. " << counterAdvisors << " were detained.";
}

void CK3::World::congregateDFCounties()
{
	auto counter = 0;
	// We're linking all contained counties for a title's tree under that title.
	// This will form actual EU4 tag and contained provinces.
	for (const auto& title: independentTitles)
	{
		title.second->congregateDFCounties();
		for (const auto& province: title.second->getOwnedDFCounties())
		{
			province.second->loadHoldingTitle(std::pair(title.first, title.second));
		}
		counter += static_cast<int>(title.second->getOwnedDFCounties().size());
	}
	Log(LogLevel::Info) << "<> " << counter << " counties held by independents.";
}

void CK3::World::congregateDJCounties()
{
	auto counter = 0;
	// We're linking all dejure provinces under the title as these will be the base
	// for that title's permanent claims, unless already owned.
	for (const auto& title: independentTitles)
	{
		title.second->congregateDJCounties();
		counter += static_cast<int>(title.second->getOwnedDJCounties().size());
	}
	Log(LogLevel::Info) << "<> " << counter << " de jure provinces claimed by independents.";
}

void CK3::World::filterLandlessTitles()
{
	auto counter = 0;
	std::set<std::string> titlesForDisposal;
	for (const auto& title: independentTitles)
	{
		if (title.second->getOwnedDFCounties().empty())
		{
			titlesForDisposal.insert(title.first);
		}
	}
	for (const auto& drop: titlesForDisposal)
	{
		independentTitles.erase(drop);
		counter++;
	}
	Log(LogLevel::Info) << "<> " << counter << " empty titles dropped, " << independentTitles.size() << " remain.";
}

void CK3::World::setElectors()
{
	// Finding electorates is not entirely trivial. CK3 has 7-XX slots, one of which is usually the Emperor himself, but
	// he is not considered an elector in EU4 sense unless he holds one of the electorate titles which are historical.
	// However, CK3 doesn't care about titles, it stores people, so a multiduke with a secondary electoral title will still
	// be elector and we need to flag his primary title as electorate one, as other duchies will possibly be annexed or PU-d.
	// Moreover, these electors may not even be indeps after HRE shattering as player may opt to keep kingdoms but electors were
	// under these kings. We can't help that.

	if (!hreTitle)
	{
		Log(LogLevel::Info) << ">< HRE does not exist.";
		return;
	}
	auto electors = hreTitle->second->getElectors();
	if (electors.empty())
	{
		Log(LogLevel::Info) << ">< HRE does not have electors.";
		return;
	}

	auto counter = 0;

	// Preambule done, we start here.
	// Make a registry of indep titles and their holders.
	std::map<long long, std::map<std::string, std::shared_ptr<Title>>> holderTitles; // holder/titles
	std::pair<long long, std::shared_ptr<Character>> hreHolder;

	for (const auto& title: independentTitles)
	{
		holderTitles[title.second->getHolder()->first].insert(title);
		if (title.second->isHREEmperor())
		{
			hreHolder = *title.second->getHolder();
		}
	}

	if (!hreHolder.first)
	{
		Log(LogLevel::Info) << ">< HRE does not have an emperor.";
		return;
	}

	// Now roll through electors and flag their primary titles as electors. If kings get electorate status
	// but kingdoms are also shattered, tough luck? Their primary duchy won't inherit electorate as they could
	// end up with multiple electorates, and that's possible only through EU4 gameplay and causes massive
	// penalties to IA.

	for (auto& elector: electors)
	{
		if (counter >= 7)
			break; // We had enough.

		if (electors.size() > 7 && elector.first == hreHolder.first)
		{
			continue; // We're skipping the emperor for 8+ slot setups.
		}

		// How many indep titles does he hold? If any?
		const auto& regItr = holderTitles.find(elector.first);
		if (regItr == holderTitles.end())
		{
			continue; // This fellow was cheated out of electorate titles.
		}

		// Which title is his primary? The first one in his domain (that survived the shattering)
		if (elector.second->getCharacterDomain() && !elector.second->getCharacterDomain()->getDomain().empty())
		{
			for (const auto& electorTitle: elector.second->getCharacterDomain()->getDomain())
			{
				// mark this title as electorate if it's independent and has land.
				if (regItr->second.count(electorTitle.second->getName()) && !electorTitle.second->getOwnedDFCounties().empty())
				{
					electorTitle.second->setElectorate();
					Log(LogLevel::Debug) << "Setting electorate: " << electorTitle.second->getName();
					counter++;
					break;
				}
			}
			// If we marked none here, then all his titles are dependent and he's not a good elector choice.
		}
		else
		{
			// This is a fellow without a domain? Mark some independent non-landless title as electorate.
			for (const auto& title: regItr->second)
			{
				if (!title.second->getOwnedDFCounties().empty())
				{
					title.second->setElectorate();
					Log(LogLevel::Debug) << "Setting electorate: " << title.first;
					counter++;
					break;
				}
			}
			// otherwise no helping this fellow.
		}
	}

	Log(LogLevel::Info) << "<> " << counter << " electorates linked.";
}
