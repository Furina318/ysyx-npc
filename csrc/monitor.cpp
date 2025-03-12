
// #include <isa.h>
#include "../include/paddr.h"
#include "../include/common.h"
#include "../include/debug.h"
#include "../include/utils.h"
#include <elf.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


void init_log(const char *log_file);
void init_mem();
void init_sdb();


static void welcome() {
  Log("MTrace: %s", MUXDEF(CONFIG_MTACE, ANSI_FMT("ON", ANSI_FG_GREEN), ANSI_FMT("OFF", ANSI_FG_RED)));
  Log("ITrace: %s", MUXDEF(CONFIG_ITRACE, ANSI_FMT("ON", ANSI_FG_GREEN), ANSI_FMT("OFF", ANSI_FG_RED)));
  Log("FTrace: %s", MUXDEF(CONFIG_FTRACE, ANSI_FMT("ON", ANSI_FG_GREEN), ANSI_FMT("OFF", ANSI_FG_RED)));
  Log("Difftest: %s", MUXDEF(CONFIG_DIFFTEST, ANSI_FMT("ON", ANSI_FG_GREEN), ANSI_FMT("OFF", ANSI_FG_RED)));
  Log("Trace: %s", MUXDEF(CONFIG_TRACE, ANSI_FMT("ON", ANSI_FG_GREEN), ANSI_FMT("OFF", ANSI_FG_RED)));
  IFDEF(CONFIG_TRACE, Log("If trace is enabled, a log file will be generated "
        "to record the trace. This may lead to a large log file. "
        "If it is not necessary, you can disable it in menuconfig"));
  Log("Build time: %s, %s", __TIME__, __DATE__);
  printf("Welcome to %s-NPC!\n", ANSI_FMT("RISCV32e", ANSI_FG_YELLOW ANSI_BG_RED));
  printf("For help, type \"help\"\n");
//   Log("Exercise: Please remove me in the source code and compile NEMU again.");
  //assert(0);
}

// #ifndef CONFIG_TARGET_AM
#include <getopt.h>

void sdb_set_batch_mode();

static char *log_file = NULL;
static char *diff_so_file = NULL;
static char *elf_file = NULL;
static char *img_file = NULL;
static int difftest_port = 1234;

static long load_img() {//load_img函数用于加载镜像文件
  if (img_file == NULL) {
    Log("No image is given. Use the default build-in image.");
    return 4096; // built-in image size
  }

  FILE *fp = fopen(img_file, "rb");
  Assert(fp, "Can not open '%s'", img_file);

  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);

  Log("The image is %s, size = %ld", img_file, size);

  fseek(fp, 0, SEEK_SET);
  int ret = fread(guest_to_host(RESET_VECTOR), size, 1, fp);
  assert(ret == 1);

  fclose(fp);
  return size;
}

//load_func_table函数用于加载ELF文件中的符号表
typedef struct {
  uint32_t addr;  // 函数地址
  uint32_t size;  // 函数大小
  char name[64];  // 函数名
} func_symbol_t;

func_symbol_t func_table[4096]; // 符号表
int func_count = 0;             // 符号数量
#define CODE_BASE_ADDR 0x80000000;  // 根据 ELF 的 Program Header 动态获取

void load_func_table(const char *elf_file) {
  FILE* fp = fopen(elf_file, "rb");
  Assert(fp, "Can not open '%s'", elf_file);
  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  Log("The ELF file is %s, size = %ld", elf_file, size);
  printf("ftrace log : The ELF file is %s, size = %ld\n", elf_file, size);

  // Read ELF header
  fseek(fp, 0, SEEK_SET);
  Elf32_Ehdr elf_header;
  assert(fread(&elf_header, sizeof(Elf32_Ehdr), 1, fp)==1);

  // Locate and read section headers
  // Elf32_Shdr shdr;
  fseek(fp, elf_header.e_shoff, SEEK_SET);
  Elf32_Shdr sh_table[elf_header.e_shnum];
  assert(fread(sh_table, sizeof(Elf32_Shdr), elf_header.e_shnum, fp)==elf_header.e_shnum);

  // Locate symbol table and string table
  Elf32_Shdr *symtab = NULL;
  Elf32_Shdr *strtab = NULL;
  for (int i = 0; i < elf_header.e_shnum; i++) {
    if (sh_table[i].sh_type == SHT_SYMTAB) {
      symtab = &sh_table[i];
    }
    if (sh_table[i].sh_type == SHT_STRTAB && i != elf_header.e_shstrndx) {
      strtab = &sh_table[i];
    }
  }
  Assert(symtab && strtab, "Failed to locate symbol table or string table");

  // Read symbol table
  Elf32_Sym symbols[symtab->sh_size / sizeof(Elf32_Sym)];
  fseek(fp, symtab->sh_offset, SEEK_SET);
  assert(fread(symbols, symtab->sh_size, 1, fp)==1);

  // Read string table
  char strtab_data[strtab->sh_size];
  fseek(fp, strtab->sh_offset, SEEK_SET);
  assert(fread(strtab_data, strtab->sh_size, 1, fp)==1);

  // Parse symbols and store function symbols in func_table
  func_count = 0;
  for (int i = 0; i < symtab->sh_size / sizeof(Elf32_Sym); i++) {
    if (ELF32_ST_TYPE(symbols[i].st_info) == STT_FUNC) {
      func_table[func_count].addr = symbols[i].st_value;
      func_table[func_count].size = symbols[i].st_size;
      strncpy(func_table[func_count].name, &strtab_data[symbols[i].st_name], sizeof(func_table[func_count].name) - 1);
      func_table[func_count].name[sizeof(func_table[func_count].name) - 1] = '\0';
      func_count++;
    }
  }

  fclose(fp);
  Log("Loaded %d function symbols from ELF file", func_count);
}

//init_log函数用于初始化日志文件
// extern uint64_t g_nr_guest_inst;

// #ifndef CONFIG_TARGET_AM
FILE *log_fp = NULL;

void init_log(const char *log_file) {
  log_fp = stdout;
  if (log_file != NULL) {
    FILE *fp = fopen(log_file, "w");
    Assert(fp, "Can not open '%s'", log_file);
    log_fp = fp;
  }
  Log("Log is written to %s", log_file ? log_file : "stdout");
}

// bool log_enable() {
//   return MUXDEF(CONFIG_TRACE, (g_nr_guest_inst >= CONFIG_TRACE_START) &&
//          (g_nr_guest_inst <= CONFIG_TRACE_END), false);
// }
// #endif



static int parse_args(int argc, char *argv[]) {
  const struct option table[] = {
    {"batch"    , no_argument      , NULL, 'b'},
    {"log"      , required_argument, NULL, 'l'},
    {"diff"     , required_argument, NULL, 'd'},
    {"elf"      , required_argument, NULL, 'e'},
    {"port"     , required_argument, NULL, 'p'},
    {"help"     , no_argument      , NULL, 'h'},
    {0          , 0                , NULL,  0 },
  };
  int o;
  while ( (o = getopt_long(argc, argv, "-bhl:d:p:e:", table, NULL)) != -1) {
    switch (o) {
      case 'b': sdb_set_batch_mode(); break;
      case 'p': sscanf(optarg, "%d", &difftest_port); break;
      case 'l': log_file = optarg; break;
      case 'd': diff_so_file = optarg; break;
      case 'e': elf_file = optarg;  break;
      case 1: img_file = optarg; return 0;
      default:
        printf("Usage: %s [OPTION...] IMAGE [args]\n\n", argv[0]);
        printf("\t-b,--batch              run with batch mode\n");
        printf("\t-l,--log=FILE           output log to FILE\n");
        printf("\t-d,--diff=REF_SO        run DiffTest with reference REF_SO\n");
        printf("\t-p,--port=PORT          run DiffTest with port PORT\n");
        printf("\t-e,--elf=ELF_FILE       load ELF file for ftrace\n");
        printf("\n");
        exit(0);
    }
  }
  return 0;
}

void init_monitor(int argc, char *argv[]) {
  /* Perform some global initialization. */

  /* Parse arguments. */
  parse_args(argc, argv);

  /* Set random seed. */
  // init_rand();

  /* Open the log file. */
  init_log(log_file);
  
  /* Load ELF file */
#ifdef CONFIG_FTRACE
    if (elf_file != NULL) {
        load_func_table(elf_file);
    }
#endif
  /* Initialize memory. */
  init_mem();

  /* Initialize devices. */
  IFDEF(CONFIG_DEVICE, init_device());

  /* Perform ISA dependent initialization. */
//   init_isa();

  /* Load the image to memory. This will overwrite the built-in image. */
  long img_size = load_img();

  /* Initialize differential testing. */
  // init_difftest(diff_so_file, img_size, difftest_port);

  /* Initialize the simple debugger. */
  init_sdb();

  IFDEF(CONFIG_ITRACE, init_disasm());

  /* Display welcome message. */
  welcome();
}
// #else // CONFIG_TARGET_AM
// static long load_img() {
//   extern char bin_start, bin_end;
//   size_t size = &bin_end - &bin_start;
//   Log("img size = %ld", size);
//   memcpy(guest_to_host(RESET_VECTOR), &bin_start, size);
//   return size;
// }

// void am_init_monitor() {
//   init_rand();
//   init_mem();
//   init_isa();
//   load_img();
//   IFDEF(CONFIG_DEVICE, init_device());
//   welcome();
// }
// #endif