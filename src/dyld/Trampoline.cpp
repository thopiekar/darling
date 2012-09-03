#include "Trampoline.h"
#include <unistd.h>
#include <sys/mman.h>
#include <stdexcept>
#include <cassert>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>

TrampolineMgr* TrampolineMgr::m_pInstance = 0;
int TrampolineMgr::m_nDepth = 0;
std::map<std::string, TrampolineMgr::FunctionInfo> TrampolineMgr::m_functionInfo;

extern "C" void reg_saveall();
extern "C" void reg_restoreall();

TrampolineMgr::TrampolineMgr(int entries)
	: m_nNext(0)
{
	assert(m_pInstance == 0);
	m_pInstance = this;

	int ps = getpagesize();
	void* mem;

	int bytes = entries * sizeof(Trampoline);
	bytes = (bytes + ps - 1) / ps * ps;

	mem = ::mmap(0, bytes, PROT_EXEC | PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	if (mem == MAP_FAILED)
	throw std::runtime_error("Failed to map pages for TrampolineMgr");

	m_pMem = static_cast<Trampoline*>(mem);
	m_nMax = bytes / sizeof(Trampoline);

}

TrampolineMgr::~TrampolineMgr()
{
	::munmap(m_pMem, m_nMax * sizeof(Trampoline));
}

void* TrampolineMgr::generate(void* targetAddr, const char* name)
{
	if ((targetAddr > m_pMem && targetAddr < m_pMem+m_nMax) || !isExecutable(targetAddr))
		return targetAddr; // will not create a trampoline for a trampoline

	AddrEntry e = { name, targetAddr };
	if (m_nNext >= m_nMax)
		throw std::runtime_error("TrampolineMgr buffer full");

	m_entries.push_back(e);
	m_pMem[m_nNext].init(m_nNext, TrampolineMgr::printInfo, TrampolineMgr::printInfoR);
	
	void* addr = &m_pMem[m_nNext++];
	// std::cout << "Trampoline for " << name << " is at " << addr << std::endl;
	return addr;
}

bool TrampolineMgr::isExecutable(void* addr)
{
	if (m_memoryMap.empty())
		loadMemoryMap();
		
	// std::cout << "isExecutable(): " << addr << std::endl;
	
	for (auto it = m_memoryMap.begin(); it != m_memoryMap.end(); it++)
	{
		if (addr >= it->start && addr < it->end)
			return it->executable;
	}
	
	return false;
}

void TrampolineMgr::invalidateMemoryMap()
{
	m_memoryMap.clear();
}

void TrampolineMgr::loadMemoryMap()
{
	std::ifstream file("/proc/self/maps");
	std::string line;
	
	while (std::getline(file, line))
	{
		if (line.empty()) continue;
		const char* s = line.c_str();
		MemoryPages pages;
		
		pages.start = (void*) strtol(s, (char**) &s, 16);
		s++;
		pages.end = (void*) strtol(s, (char**) &s, 16);
		pages.executable = (*(s+3) == 'x') && (*(s+2) == '-'); // not writable
		
		//std::cout << line << " -> " << pages.start << " - " << pages.end << " " << pages.executable << std::endl;
		m_memoryMap.push_back(pages);
	}
}

void TrampolineMgr::loadFunctionInfo(const char* path)
{
	std::ifstream file(path);
	std::string line;
	
	while (std::getline(file, line))
	{
		size_t p = line.find(':');
		if (p == std::string::npos)
			continue;
		
		FunctionInfo info;
		info.retType = line.at(p+1);
		info.arguments = line.substr(p+2);
		
		m_functionInfo[line.substr(0, p)] = info;
	}
}

void* TrampolineMgr::printInfo(uint32_t index, CallStack* stack)
{	
	FunctionInfo* info = 0;
	const std::string& name = m_pInstance->m_entries[index].name;
	auto it = m_functionInfo.find(name);
	
	std::cerr << std::string(m_nDepth, ' ');
	
	if (it != m_functionInfo.end())
	{
		ArgumentWalker w(stack);
		bool first = true;
		
		std::cerr << name << '(';
		
		for (char c : it->second.arguments)
		{
			if (!first)
				std::cerr << ", ";
			else
				first = false;
			
			std::cerr << w.next(c);
		}
		std::cerr << ")\n" << std::flush;
	}
	else
		std::cerr << m_pInstance->m_entries[index].name << "(?)\n" << std::flush;
	m_pInstance->m_entries[index].retAddr = stack->retAddr;
	
	m_nDepth++;
	
	return m_pInstance->m_entries[index].addr;
}

void* TrampolineMgr::printInfoR(uint32_t index, CallStack* stack)
{
	void* rv = m_pInstance->m_entries[index].retAddr;
	
	m_pInstance->m_entries[index].retAddr = 0;
	m_nDepth--;
	
	const std::string& name = m_pInstance->m_entries[index].name;
	auto it = m_functionInfo.find(name);
	
	std::cerr << std::string(m_nDepth, ' ');
	
	if (it != m_functionInfo.end())
	{
		ArgumentWalker w(stack);
		std::cerr << "-> " << w.ret(it->second.retType) << '\n' << std::flush;
	}
	else
		std::cerr << "-> ?\n" << std::flush;
	
	// standard retval in rax, double in xmm0
	return rv;
}

void Trampoline::init(uint32_t i, void* (*pDebug)(uint32_t,TrampolineMgr::CallStack*), void* (*pDebugR)(uint32_t,TrampolineMgr::CallStack*))
{
	// See trampoline in trampoline_helper.asm for source
	memcpy(this, "\x49\xba\xb6\xb5\xb4\xb3\xb2\xb1\xb0\x00\x41\xff\xd2\xbf"
		"\x56\x34\x12\x00\x48\x89\xe6\x48\xb9\xff\xee\xdd\xcc\xbb\xaa\x00\x00"
		"\xff\xd1\x49\x89\xc3\x49\xba\xc6\xc5\xc4\xc3\xc2\xc1\xc0\x00\x41\xff"
		"\xd2\x4c\x8d\x15\x08\x00\x00\x00\x4c\x89\x14\x24\x41\xff\xe3\x90\x49"
		"\xba\xb6\xb5\xb4\xb3\xb2\xb1\xb0\x00\x41\xff\xd2\xbf\x56\x34\x12\x00"
		"\x48\x89\xe6\x48\xb9\xa6\xa5\xa4\xa3\xa2\xa1\xa0\x00\xff\xd1\x49\x89"
		"\xc3\x49\xba\xc6\xc5\xc4\xc3\xc2\xc1\xc0\x00\x41\xff\xd2\x41\xff\xe3",
		sizeof(*this)
	);
	
	this->reg_saveall = reinterpret_cast<uint64_t>(::reg_saveall);
	this->reg_saveall2 = reinterpret_cast<uint64_t>(::reg_saveall);
	this->reg_restoreall = reinterpret_cast<uint64_t>(::reg_restoreall);
	this->reg_restoreall2 = reinterpret_cast<uint64_t>(::reg_restoreall);
	this->index = i;
	this->index2 = i;
	this->debugFcn = reinterpret_cast<uint64_t>(pDebug);
	this->debugFcnR = reinterpret_cast<uint64_t>(pDebugR);
}

TrampolineMgr::ArgumentWalker::ArgumentWalker(CallStack* stack)
: m_stack(stack), m_indexInt(0), m_indexXmm(0)
{
}

TrampolineMgr::ArgumentWalker::ArgumentWalker(CallStack* stack, OutputArguments args)
: m_stack(stack), m_indexInt(0), m_indexXmm(0), m_pointers(args)
{
}


uint64_t TrampolineMgr::ArgumentWalker::next64bit()
{
	uint64_t rv;
	if (m_indexInt == 0)
		rv = m_stack->rdi;
	else if (m_indexInt == 1)
		rv = m_stack->rsi;
	else if (m_indexInt == 2)
		rv = m_stack->rdx;
	else if (m_indexInt == 3)
		rv = m_stack->rcx;
	else if (m_indexInt == 4)
		rv = m_stack->r8;
	else if (m_indexInt == 5)
		rv = m_stack->r9;
	else
		throw std::out_of_range("7th int argument not supported");
	
	m_indexInt++;
	return rv;
}

long double TrampolineMgr::ArgumentWalker::nextDouble()
{
	long double rv;
	if (m_indexXmm >= 0 && m_indexXmm <= 7)
		return m_stack->xmm[m_indexXmm];
	else
		throw std::out_of_range("8th double argument not supported");
	
	m_indexXmm++;
	return rv;
}

std::string TrampolineMgr::ArgumentWalker::next(char type)
{
	std::stringstream ss;
	void* ptr;
	
	if (type == 'u')
		ss << next64bit();
	else if (type == 'i')
	{
		uint64_t u = next64bit();
		ss << *((int64_t*) &u);
	}
	else if (type == 'f')
	{
		long double d = nextDouble();
		ss << *((float*)&d);
	}
	else if (type == 'd')
	{
		long double d = nextDouble();
		ss << *((double*)&d);
	}
	else if (type == 'p' || isupper(type))
	{
		ptr = (void*) next64bit();
		ss << ptr;
	}
	else if (type == 'c')
		ss << char(next64bit());
	else if (type == 's')
	{
		const char* s = (const char*) next64bit();
		ss << (void*)s;
		if (s)
			ss << " \"" << safeString(s) << '"';
	}
	else if (type == 'v')
		ss << "(void)";
	else
		ss << '?';
		
	if (isupper(type))
		m_pointers.push_back(std::make_pair(tolower(type), ptr));
	
	return ss.str();
}

std::string TrampolineMgr::ArgumentWalker::ret(char type)
{
	std::stringstream ss;
	
	if (type == 'u')
		ss << m_stack->rax;
	else if (type == 'c')
		ss << char(m_stack->rax);
	else if (type == 'i')
	{
		uint64_t u = m_stack->rax;
		ss << *((int64_t*) &u);
	}
	else if (type == 'f')
	{
		long double d = m_stack->xmm[0];
		ss << *((float*)&d);
	}
	else if (type == 'd')
	{
		long double d = m_stack->xmm[0];
		ss << *((double*)&d);
	}
	else if (type == 'p' || isupper(type))
		ss << (void*)m_stack->rax << std::dec;
	else if (type == 's')
	{
		const char* s = (const char*) m_stack->rax;
		ss << (void*)s << " \"" <<  safeString(s) << '"';
	}
	else
		ss << '?';
	
	return ss.str();
}

std::string TrampolineMgr::ArgumentWalker::safeString(const char* in)
{
	if (!in)
		return std::string();
	
	std::stringstream rv;
	while (*in)
	{
		if (*in >= 32)
			rv << *in;
		else
		{
			if (*in == '\n')
				rv << "\\n";
			else if (*in == '\r')
				rv << "\\r";
			else if (*in == '\t')
				rv << "\\t";
			else
				rv << "\\x" << std::hex << int(*in);
		}
		in++;
	}
	return rv.str();
}

#ifdef TEST

double mytestfunc(int a, int b, double c)
{
	return a*b+c;
}

int main()
{
	TrampolineMgr* mgr = new TrampolineMgr;
	TrampolineMgr::loadFunctionInfo("/tmp/fi");
	
	double (*pFunc)(int,int,double) = (double (*)(int,int,double)) mgr->generate((void*) &mytestfunc, "mytestfunc");
	int (*pPrintf)(FILE* f, const char*,...) = (int (*)(FILE* f, const char*,...)) mgr->generate((void*) &fprintf, "printf");
	//std::cout << pFunc(2,3,0.5) << std::endl;
	pPrintf(stdout, "Hello world: %s\n", "Test");
	
	delete mgr;
	return 0;
}

#endif
