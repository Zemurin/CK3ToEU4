#include "Dynasties.h"
#include "../CoatsOfArms/CoatsOfArms.h"
#include "CommonRegexes.h"
#include "Dynasty.h"
#include "Log.h"
#include "ParserHelpers.h"

CK3::Dynasties::Dynasties(std::istream& theStream)
{
	registerKeys();
	parseStream(theStream);
	clearRegisteredKeywords();
}

void CK3::Dynasties::registerKeys()
{
	registerKeyword("dynasty_house", [this](const std::string& unused, std::istream& theStream) {
		houses = Houses(theStream);
	});
	registerKeyword("dynasties", [this](const std::string& unused, std::istream& theStream) {
		dynasties = Dynasties(theStream).getDynasties();
	});
	registerRegex(R"(\d+)", [this](const std::string& gameID, std::istream& theStream) {
		const auto suspiciousItem = commonItems::stringOfItem(theStream).getString();
		if (suspiciousItem.find('{') != std::string::npos)
		{
			auto dynastyStream = std::stringstream(suspiciousItem);
			const auto newDynasty = std::make_shared<Dynasty>(dynastyStream, std::stoll(gameID));
			dynasties.insert(std::pair(newDynasty->getGameID(), newDynasty));
		}
	});
	registerRegex(commonItems::catchallRegex, commonItems::ignoreItem);
}

void CK3::Dynasties::linkCoats(const CoatsOfArms& coats)
{
	auto counter = 0;
	const auto& coatData = coats.getCoats();
	for (const auto& dynasty: dynasties)
	{
		if (!dynasty.second->getCoA())
			continue;
		const auto& coatDataItr = coatData.find(dynasty.second->getCoA()->first);
		if (coatDataItr != coatData.end())
		{
			dynasty.second->loadCoat(*coatDataItr);
			++counter;
		}
		else
		{
			Log(LogLevel::Warning) << "Dynasty " + std::to_string(dynasty.first) + " has CoA " + std::to_string(dynasty.second->getCoA()->first) +
													" which has no definition!";
		}
	}
	Log(LogLevel::Info) << "<> " << counter << " dynasties updated.";
}
