#include "utility/IncludeProcessing.h"

#include <set>
#include <unordered_set>

#include "utility/IncludeDirective.h"
#include "utility/file/FilePath.h"
#include "utility/file/FileTree.h"
#include "utility/text/TextAccess.h"
#include "utility/utility.h"
#include "utility/utilityString.h"

namespace
{
	struct IncludeDirectiveComparator
	{
		bool operator()(const IncludeDirective& a, const IncludeDirective& b)
		{
			return a.getIncludedFile() < b.getIncludedFile();
		}
	};

	std::vector<std::vector<FilePath>> splitToQuantiles(
		const std::set<FilePath>& sourceFilePaths, 
		const size_t desiredQuantileCount)
	{
		size_t quantileCount = std::max<size_t>(1, std::min(desiredQuantileCount, sourceFilePaths.size()));

		std::vector<std::vector<FilePath>> quantiles;
		for (size_t i = 0; i < quantileCount; i++)
		{
			quantiles.push_back(std::vector<FilePath>());
		}

		int i = 0;
		for (const FilePath& sourceFilePath : sourceFilePaths)
		{
			quantiles[i % quantileCount].push_back(sourceFilePath);
			++i;
		}

		return quantiles;
	}
}

std::vector<IncludeDirective> IncludeProcessing::getUnresolvedIncludeDirectives(
	const std::set<FilePath>& sourceFilePaths,
	const std::set<FilePath>& indexedPaths,
	const std::set<FilePath>& headerSearchDirectories,
	const size_t desiredQuantileCount, std::function<void(float)> progress
)
{
	std::unordered_set<std::string> processedFilePaths;
	std::set<IncludeDirective, IncludeDirectiveComparator> unresolvedIncludeDirectives;

	std::vector<std::vector<FilePath>> quantiles = splitToQuantiles(sourceFilePaths, desiredQuantileCount);

	for (size_t i = 0; i < quantiles.size(); i++)
	{
		progress(float(i) / quantiles.size());

		const std::vector<IncludeDirective> directives = doGetUnresolvedIncludeDirectives(
			utility::toSet(quantiles[i]),
			processedFilePaths,
			indexedPaths,
			headerSearchDirectories
		);
		std::copy(directives.begin(), directives.end(), std::inserter(unresolvedIncludeDirectives, unresolvedIncludeDirectives.end()));
	}

	std::vector<IncludeDirective> ret;

	for (const IncludeDirective& directive: unresolvedIncludeDirectives)
	{
		ret.push_back(directive);
	}

	progress(1.0f);

	return ret;
}

std::set<FilePath> IncludeProcessing::getHeaderSearchDirectories(
	const std::set<FilePath>& sourceFilePaths,
	const std::set<FilePath>& searchedPaths,
	const std::set<FilePath>& currentHeaderSearchDirectories,
	const size_t desiredQuantileCount, std::function<void(float)> progress
)
{
	progress(0.0f);

	std::vector<std::shared_ptr<FileTree>> existingFileTrees;
	for (const FilePath& searchedPath : searchedPaths)
	{
		existingFileTrees.push_back(std::make_shared<FileTree>(searchedPath));
	}

	std::set<FilePath> headerSearchDirectories;
	std::unordered_set<std::string> processedFilePaths;
	std::vector<std::vector<FilePath>> quantiles = splitToQuantiles(sourceFilePaths, desiredQuantileCount);

	for (size_t i = 0; i < quantiles.size(); i++)
	{
		progress(float(i) / quantiles.size());

		std::set<FilePath> unprocessedFilePaths(quantiles[i].begin(), quantiles[i].end());

		while (!unprocessedFilePaths.empty())
		{
			std::transform(
				unprocessedFilePaths.begin(), unprocessedFilePaths.end(),
				std::inserter(processedFilePaths, processedFilePaths.begin()),
				[](const FilePath& p) { return p.getAbsolute().str(); }
			);

			std::set<FilePath> unprocessedFilePathsForNextIteration;

			for (const FilePath& unprocessedFilePath : unprocessedFilePaths)
			{
				for (const IncludeDirective& includeDirective : getIncludeDirectives(unprocessedFilePath))
				{
					const FilePath includedFilePath = includeDirective.getIncludedFile();

					FilePath foundIncludedPath = resolveIncludeDirective(includeDirective, currentHeaderSearchDirectories);
					if (foundIncludedPath.empty())
					{
						for (std::shared_ptr<FileTree> existingFileTree : existingFileTrees)
						{
							// TODO: handle the case where a file can be found by two different paths
							const FilePath rootPath = existingFileTree->getAbsoluteRootPathForRelativeFilePath(includedFilePath);
							if (!rootPath.empty())
							{
								foundIncludedPath = rootPath.getConcatenated(includedFilePath);
								if (foundIncludedPath.exists())
								{
									headerSearchDirectories.insert(rootPath);
									break;
								}
							}
						}
					}
					if (foundIncludedPath.exists())
					{
						if (processedFilePaths.find(foundIncludedPath.str()) == processedFilePaths.end())
						{
							unprocessedFilePathsForNextIteration.insert(foundIncludedPath);
						}
					}
				}
			}

			unprocessedFilePaths = unprocessedFilePathsForNextIteration;
		}
	}

	progress(1.0f);

	return headerSearchDirectories;
}

std::vector<IncludeDirective> IncludeProcessing::getIncludeDirectives(const FilePath& filePath)
{
	if (filePath.exists())
	{
		return getIncludeDirectives(TextAccess::createFromFile(filePath));
	}
	return std::vector<IncludeDirective>();
}

std::vector<IncludeDirective> IncludeProcessing::getIncludeDirectives(std::shared_ptr<TextAccess> textAccess)
{
	std::vector<IncludeDirective> includeDirectives;

	const std::vector<std::string> lines = textAccess->getAllLines();
	for (size_t i = 0; i < lines.size(); i++)
	{
		const std::string lineTrimmedToHash = utility::trim(lines[i]);
		if (utility::isPrefix<std::string>("#", lineTrimmedToHash))
		{
			const std::string lineTrimmedToInclude = utility::trim(lineTrimmedToHash.substr(1));
			if (utility::isPrefix<std::string>("include", lineTrimmedToInclude))
			{
				std::string includeString = utility::substrBetween(lineTrimmedToInclude, "<", ">");
				bool usesBrackets = true;
				if (includeString.empty())
				{
					includeString = utility::substrBetween(lineTrimmedToInclude, "\"", "\"");
					usesBrackets = false;
				}

				if (!includeString.empty())
				{
					// lines are 1 based
					includeDirectives.push_back(IncludeDirective(FilePath(includeString), textAccess->getFilePath(), i + 1, usesBrackets));
				}
			}
		}
	}

	return includeDirectives;
}

std::vector<IncludeDirective> IncludeProcessing::doGetUnresolvedIncludeDirectives(
	std::set<FilePath> filePathsToProcess,
	std::unordered_set<std::string>& processedFilePaths,
	const std::set<FilePath>& indexedPaths,
	const std::set<FilePath>& headerSearchDirectories
)
{
	std::vector<IncludeDirective> unresolvedIncludeDirectives;

	while (!filePathsToProcess.empty())
	{
		std::transform(
			filePathsToProcess.begin(), filePathsToProcess.end(),
			std::inserter(processedFilePaths, processedFilePaths.begin()),
			[](const FilePath& p) { return p.getAbsolute().makeCanonical().str(); }
		);

		std::set<FilePath> filePathsToProcessForNextIteration;

		for (const FilePath& filePath : filePathsToProcess)
		{
			for (const IncludeDirective& includeDirective : getIncludeDirectives(filePath))
			{
				const FilePath resolvedIncludePath = resolveIncludeDirective(includeDirective, headerSearchDirectories).makeCanonical();
				if (resolvedIncludePath.empty())
				{
					unresolvedIncludeDirectives.push_back(includeDirective);
				}
				else if (processedFilePaths.find(resolvedIncludePath.str()) == processedFilePaths.end())
				{
					for (const FilePath& indexedPath : indexedPaths)
					{
						if (indexedPath.contains(resolvedIncludePath))
						{
							filePathsToProcessForNextIteration.insert(resolvedIncludePath);
							break;
						}
					}
				}
			}
		}
		filePathsToProcess.clear();
		filePathsToProcess.swap(filePathsToProcessForNextIteration);
	}

	return unresolvedIncludeDirectives;
}

FilePath IncludeProcessing::resolveIncludeDirective(
	const IncludeDirective& includeDirective, 
	const std::set<FilePath>& headerSearchDirectories
)
{
	const FilePath includedFilePath = includeDirective.getIncludedFile();

	{
		// check for an absolute include path
		if (includedFilePath.isAbsolute())
		{
			const FilePath resolvedIncludePath = includedFilePath;
			if (resolvedIncludePath.exists())
			{
				return includedFilePath;
			}
		}
	}

	{
		// check for an include path relative to the including path
		const FilePath resolvedIncludePath = includeDirective.getIncludingFile().getParentDirectory().concatenate(includedFilePath);
		if (resolvedIncludePath.exists())
		{
			return resolvedIncludePath;
		}
	}

	{
		// check for an include path relative to the header search directories
		for (const FilePath& headerSearchDirectory: headerSearchDirectories)
		{
			const FilePath resolvedIncludePath = headerSearchDirectory.getConcatenated(includedFilePath);
			if (resolvedIncludePath.exists())
			{
				return resolvedIncludePath;
			}
		}
	}

	return FilePath();
}
