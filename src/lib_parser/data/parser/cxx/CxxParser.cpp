#include "data/parser/cxx/CxxParser.h"

#include "clang/Tooling/Tooling.h"

#include "utility/file/FileRegister.h"
#include "utility/logging/logging.h"
#include "utility/text/TextAccess.h"

#include "data/parser/cxx/ASTActionFactory.h"
#include "data/parser/cxx/CxxCompilationDatabaseSingle.h"
#include "data/parser/cxx/CxxDiagnosticConsumer.h"

namespace
{
	static std::vector<std::string> getSyntaxOnlyToolArgs(const std::vector<std::string> &ExtraArgs, llvm::StringRef FileName)
	{
		std::vector<std::string> Args;
		Args.push_back("clang-tool");
		Args.push_back("-fsyntax-only");
		Args.insert(Args.end(), ExtraArgs.begin(), ExtraArgs.end());
		Args.push_back(FileName.str());
		return Args;
	}

	// custom implementation of clang::runToolOnCodeWithArgs which also sets our custon DiagnosticConsumer
	static bool runToolOnCodeWithArgs(
		clang::DiagnosticConsumer* DiagConsumer,
		clang::FrontendAction *ToolAction,
		const llvm::Twine &Code,
		const std::vector<std::string> &Args,
		const llvm::Twine &FileName = "input.cc",
		const clang::tooling::FileContentMappings &VirtualMappedFiles = clang::tooling::FileContentMappings()
	)
	{
		llvm::SmallString<16> FileNameStorage;
		llvm::StringRef FileNameRef = FileName.toNullTerminatedStringRef(FileNameStorage);
		llvm::IntrusiveRefCntPtr<clang::FileManager> Files(new clang::FileManager(clang::FileSystemOptions()));
		clang::tooling::ToolInvocation Invocation(getSyntaxOnlyToolArgs(Args, FileNameRef), ToolAction, Files.get());

		llvm::SmallString<1024> CodeStorage;
		Invocation.mapVirtualFile(FileNameRef, Code.toNullTerminatedStringRef(CodeStorage));

		for (auto &FilenameWithContent : VirtualMappedFiles)
		{
			Invocation.mapVirtualFile(FilenameWithContent.first, FilenameWithContent.second);
		}

		Invocation.setDiagnosticConsumer(DiagConsumer);

		return Invocation.run();
	}
}


CxxParser::CxxParser(ParserClient* client, std::shared_ptr<FileRegister> fileRegister)
	: Parser(client)
	, m_fileRegister(fileRegister)
{
}

CxxParser::~CxxParser()
{
}

void CxxParser::parseFiles(const std::vector<FilePath>& filePaths, const Arguments& arguments)
{
	m_fileRegister->setFilePaths(filePaths);
	setupParsing(arguments);

	std::vector<std::string> sourcePaths;
	for (const FilePath& path : m_fileRegister->getUnparsedSourceFilePaths()) // filter headers
	{
		sourcePaths.push_back(path.absolute().str());
	}

	runTool(sourcePaths);
}

void CxxParser::parseFile(const FilePath& filePath, std::shared_ptr<TextAccess> textAccess, const Arguments& arguments)
{
	m_fileRegister->setFilePaths(std::vector<FilePath>(1, filePath));
	setupParsing(arguments);

	std::vector<std::string> args = getCommandlineArguments(arguments);
	std::shared_ptr<CxxDiagnosticConsumer> diagnostics = getDiagnostics(arguments);

	ASTActionFactory actionFactory(m_client, m_fileRegister.get());
	runToolOnCodeWithArgs(diagnostics.get(), actionFactory.create(), textAccess->getText(), args);
}

std::vector<std::string> CxxParser::getCommandlineArgumentsEssential(const Arguments& arguments) const
{
	std::vector<std::string> args;

	// verbose
	// args.push_back("-v");

	// The option -fno-delayed-template-parsing signals that templates that there should
	// be AST elements for unused template functions as well.
	args.push_back("-fno-delayed-template-parsing");

	// The option -fexceptions signals that clang should watch out for exception-related code during analysis.
	args.push_back("-fexceptions");

	// The option -c signals that no executable is built.
	args.push_back("-c");

	args.insert(args.begin(), arguments.compilerFlags.begin(), arguments.compilerFlags.end());

	for (const FilePath& path : arguments.headerSearchPaths)
	{
		args.push_back("-I" + path.str());
	}

	for (const FilePath& path : arguments.systemHeaderSearchPaths)
	{
		args.push_back("-isystem");
		args.push_back(path.str());
	}

	for (const FilePath& path : arguments.frameworkSearchPaths)
	{
		args.push_back("-iframework");
		args.push_back(path.str());
	}

	return args;
}

std::vector<std::string> CxxParser::getCommandlineArguments(const Arguments& arguments) const
{
	std::vector<std::string> args = getCommandlineArgumentsEssential(arguments);

	// The option '-x c++' treats subsequent input files as C++.
	args.push_back("-x");
	std::string language = getLanguageArgument(arguments.language);
	args.push_back(language);

	// Set language standard
	std::string standard = "-std=";
	standard += language;
	standard += arguments.languageStandard;
	args.push_back(standard);

	return args;
}

std::shared_ptr<clang::tooling::FixedCompilationDatabase> CxxParser::getCompilationDatabase(
	const Arguments& arguments
) const {
	// Commandline flags passed to the programm. Everything after '--' will be interpreted by the ClangTool.
	std::vector<std::string> args = getCommandlineArguments(arguments);
	args.insert(args.begin(), "app");
	args.insert(args.begin() + 1, "--");

	int argc = args.size();
	const char** argv = new const char*[argc];
	for (size_t i = 0; i < args.size(); i++)
	{
		argv[i] = args[i].c_str();
	}

	std::shared_ptr<clang::tooling::FixedCompilationDatabase> compilationDatabase(
		clang::tooling::FixedCompilationDatabase::loadFromCommandLine(argc, argv)
	);

	delete[] argv;

	if (!compilationDatabase)
	{
		LOG_ERROR("Failed to load compilation database");
		return nullptr;
	}

	return compilationDatabase;
}

std::shared_ptr<CxxDiagnosticConsumer> CxxParser::getDiagnostics(const Arguments& arguments) const
{
	llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> options = new clang::DiagnosticOptions();
	return std::make_shared<CxxDiagnosticConsumer>(
		llvm::errs(), &*options, m_client, m_fileRegister->getFileManager(), arguments.logErrors);
}

void CxxParser::setupParsing(const Arguments& arguments)
{
	m_compilationDatabase = getCompilationDatabase(arguments);
	m_diagnostics = getDiagnostics(arguments);
}

void CxxParser::setupParsingCDB(const Arguments& arguments)
{
	m_diagnostics = getDiagnostics(arguments);
}

void CxxParser::runTool(const std::vector<std::string>& files)
{
	clang::tooling::ClangTool tool(*m_compilationDatabase, files);
	tool.setDiagnosticConsumer(m_diagnostics.get());

	ASTActionFactory actionFactory(m_client, m_fileRegister.get());
	tool.run(&actionFactory);
}

void CxxParser::runTool(clang::tooling::CompileCommand command, const Arguments& arguments)
{
	std::vector<std::string> args = getCommandlineArgumentsEssential(arguments);
	command.CommandLine.insert(command.CommandLine.end(), args.begin(), args.end());

	CxxCompilationDatabaseSingle compilationDatabase(command);
	clang::tooling::ClangTool tool(compilationDatabase, std::vector<std::string>(1, command.Filename));
	tool.setDiagnosticConsumer(m_diagnostics.get());

	ASTActionFactory actionFactory(m_client, m_fileRegister.get());
	tool.run(&actionFactory);
}

FileRegister* CxxParser::getFileRegister()
{
	return m_fileRegister.get();
}

ParserClient* CxxParser::getParserClient()
{
	return m_client;
}

std::string CxxParser::getLanguageArgument(const std::string& language) const
{
	std::string result = language;

	boost::algorithm::to_lower(result);

	if (result != "c++" && result != "c")
	{
		result = "c++";
	}

	return result;
}
