// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef MAN_ENTRIES_H
#define MAN_ENTRIES_H

#include "fat32.h"
#include <stddef.h>

static size_t man_strlen(const char *str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

static void write_man_file(const char *name, const char *content) {
    char path[128] = "/Library/man/";
    int i = 13;
    while (*name) path[i++] = *name++;
    path[i++] = '.';
    path[i++] = 't';
    path[i++] = 'x';
    path[i++] = 't';
    path[i] = 0;

    FAT32_FileHandle *fh = fat32_open(path, "w");
    if (fh) {
        fat32_write(fh, (void *)content, man_strlen(content));
        fat32_close(fh);
    }
}

void create_man_entries(void) {
    fat32_mkdir("/Library");
    fat32_mkdir("/Library/man");

    write_man_file("ping", "PING - Send ICMP echo requests\n\nUsage: ping <ip>\n\nSends ICMP echo requests to the specified IP address and displays the response times.");
    write_man_file("net", "NET - Network utilities\n\nUsage: net init\nnet info\nnet ipset >ip<\nnet udpsend >ip< >port< >message< net ping >ip< net help\n\nA collection of network-related commands.");
    write_man_file("ls", "LS - List directory contents\n\nUsage: ls [path]\n\nLists files and directories in the current or specified directory.");
    write_man_file("cat", "CAT - Concatenate and display file contents\n\nUsage: cat <filename>\n\nDisplays the text content of the specified file.");
    write_man_file("man", "MAN - Display manual pages\n\nUsage: man <command>\n\nDisplays help information for the specified command.");
    write_man_file("clear", "CLEAR - Clear terminal screen\n\nUsage: clear\n\nClears all text from the current terminal window.");
    write_man_file("date", "DATE - Show current date and time\n\nUsage: date\n\nDisplays the current system date and time from the RTC.");
    write_man_file("echo", "ECHO - Print text\n\nUsage: echo [text]\n\nPrints the specified text to the terminal.");
    write_man_file("hello", "HELLO - Hello World demo\n\nUsage: hello\n\nA simple demonstration program that prints a greeting.");
    write_man_file("help", "HELP - List available commands\n\nUsage: help\n\nLists all internal and external commands available in the shell.");
    write_man_file("kill", "KILL - Terminate a process\n\nUsage: kill [-9] <pid>\n\nSends SIGTERM by default or SIGKILL with -9.");
    write_man_file("uptime", "UPTIME - Show system uptime\n\nUsage: uptime\n\nDisplays how long BoredOS has been running since boot.");
    write_man_file("pwd", "PWD - Print working directory\n\nUsage: pwd\n\nDisplays the absolute path of the current working directory.");
    write_man_file("mkdir", "MKDIR - Create directory\n\nUsage: mkdir <dirname>\n\nCreates a new directory with the specified name.");
    write_man_file("rm", "RM - Remove file\n\nUsage: rm <filename>\n\nDeletes the specified file from the filesystem.");
    write_man_file("mv", "MV - Move or rename file\n\nUsage: mv <source> <dest>\n\nMoves or renames a file or directory.");
    write_man_file("cp", "CP - Copy file\n\nUsage: cp <source> <dest>\n\nCopies a file from the source path to the destination path.");
    write_man_file("touch", "TOUCH - Create empty file\n\nUsage: touch <filename>\n\nCreates a new empty file if it doesn't exist.");
    write_man_file("cc", "CC - C Compiler\n\nUsage: cc <file.c>\n\nThe BoredOS C Compiler. Compiles C source files into executables. (execute these with ./>file<)");
    write_man_file("crash", "CRASH - Trigger kernel exception\n\nUsage: crash\n\nIntentionally triggers a null pointer dereference to test handlers.");
    write_man_file("sysfetch", "SYSFETCH - Show OS information\n\nUsage: sysfetch\n\nDisplays system information in a neofetch-like layout. Configurable via /Library/conf/sysfetch.cfg.");
    write_man_file("uname", "UNAME - Print system information\n\nUsage: uname [-amnoprsv]\n\nOptions:\n  -a  Print all information\n  -s  Kernel name\n  -n  Node name\n  -r  Kernel release\n  -v  Kernel build date and time\n  -m  Machine hardware name\n  -p  Processor type\n  -o  Operating system name");
    write_man_file("meminfo", "MEMINFO - Memory usage stats\n\nUsage: meminfo\n\nDisplays current physical and virtual memory allocation statistics.");
    write_man_file("pci_list", "PCI_LIST - Scan PCI bus\n\nUsage: pci_list\n\nScans the PCI bus and lists all detected hardware devices.");
    write_man_file("reboot", "REBOOT - Restart system\n\nUsage: reboot\n\nRestarts the computer immediately.");
    write_man_file("shutdown", "SHUTDOWN - Power off\n\nUsage: shutdown\n\nPowers off the machine (requires ACPI support).");
    write_man_file("txtedit", "TXTEDIT - Terminal text editor\n\nUsage: txtedit <filename>\n\nOpens a CLI-based text editor within the terminal.");
    write_man_file("math", "MATH - Expression evaluator\n\nUsage: math <expression>\n\nEvaluates simple arithmetic expressions from the command line.");
}

#endif
