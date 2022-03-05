#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <switch.h>

#include "log.h"

static char elfPath[784];

//Menu state
static const char* currentDevice = NULL;
static char currentPath[784] = {0};
static s64 currentlySelectedEntryIndex = 0;
static s64 currentDirectoryEntriesNum = 0;
static FsDirectoryEntry* currentDirectoryContents = NULL;
static unsigned padCooldown = 0; //TODO make this a proper input system
#define PAD_COOLDOWN 15

//Returns number of directory entries on success, -1 on error
s64 GetDirectoryEntries(const char* device, const char* directory, FsDirectoryEntry** ppDirEntries) {
	if (!device || !directory || !ppDirEntries) {
        printf("%s:invalid parameters (%p %p %p)", __func__, device, directory, ppDirEntries);
        return -1;
    }
    
    FsFileSystem* devFs = fsdevGetDeviceFileSystem(device);
	if (!devFs) {
		printf("fsdevGetDeviceFileSystem(%s) failed.", device);
		return -1;
	}

    FsDir rootDir;
	Result res = fsFsOpenDirectory(devFs, directory, FsOpenMode_Read, &rootDir);
	if (R_FAILED(res)) {
		printf("fsFsOpenDirectory(%s) failed: 0x%lx", directory, res);
		return -1;
	}

	s64 entriesCount = 0;
    res = fsDirGetEntryCount(&rootDir, &entriesCount);
	if (R_FAILED(res)) {
		printf("fsDirGetEntryCount failed: 0x%lx", res);
		goto err_close_root_dir;
	}

    FsDirectoryEntry* dirEntries = malloc(entriesCount * sizeof(FsDirectoryEntry));
    if (!dirEntries) {
        puts("Failed to allocate memory for root directory entries.");
        goto err_close_root_dir;
    }

    
    s64 readEntries = 0;
    res = fsDirRead(&rootDir, &readEntries, entriesCount, dirEntries);
    if (R_FAILED(res)) {
        LOG("Failed to read directory entries: 0x%lx", res);
        goto err_free_dir_entries;
    }

    fsDirClose(&rootDir);
    *ppDirEntries = dirEntries;
    return readEntries;

err_free_dir_entries:
    free(dirEntries);

err_close_root_dir:
    fsDirClose(&rootDir);

	return -1;
}

//Assumes pad has not been initialized
//Initializes its own console, and exits it
//returns path to elf file (without leading device:) on success, NULL on failure
const char* SelectElfMenu(const char* device) {
    //Initialize menu state
    currentDevice = device;
    currentPath[0] = '/'; //Initial path - root of device (/)
    currentPath[1] = '\0';

    currentDirectoryEntriesNum = GetDirectoryEntries(device, currentPath, &currentDirectoryContents); //Read root directory
    if (currentDirectoryEntriesNum < 0) {
        return NULL;
    }

    PrintConsole* console = consoleInit(NULL);
    consoleSelect(console);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);    
    PadState pad;
    padInitializeDefault(&pad);

    while (1) {
        consoleClear();
        padUpdate(&pad);
        const u64 buttons = padGetButtons(&pad);

        //TODO make a good input system...
        //this works for now
        if (padCooldown == 0) {
            if (buttons != 0) {
                padCooldown = PAD_COOLDOWN;
                if ((buttons & HidNpadButton_AnyUp) && (currentlySelectedEntryIndex > 0))
                    currentlySelectedEntryIndex--;

                if ((buttons & HidNpadButton_AnyDown) && (currentlySelectedEntryIndex < (currentDirectoryEntriesNum-1))) //Max value allowed is num-1 since this is an index
                    currentlySelectedEntryIndex++;

                else if (buttons & HidNpadButton_A) {
                    if (currentDirectoryContents[currentlySelectedEntryIndex].type == FsDirEntryType_File) {
                        break;
                    } else {
                        strncat(currentPath, currentDirectoryContents[currentlySelectedEntryIndex].name, sizeof(elfPath)-1);
                        elfPath[sizeof(elfPath) - 1] = '\0'; //enforce strnlen returning a valid index
                        size_t idx = strnlen(currentPath, sizeof(elfPath));
                        elfPath[idx] = '/';
                        elfPath[sizeof(elfPath) - 1] = '\0';

                        //free old directory
                        free(currentDirectoryContents);

                        //load new directory and reset menu state
                        currentlySelectedEntryIndex = 0;
                        currentDirectoryEntriesNum = GetDirectoryEntries(device, currentPath, &currentDirectoryContents);
                        if (currentDirectoryEntriesNum < 0) {
                            consoleClear();
                            printf("Failed to open '%s:%s'\n", device, currentPath);
                            consoleUpdate(console);
                            consoleExit(console);
                            return NULL;
                        }
                    }
                } else if ((buttons & HidNpadButton_B) && (currentPath[1] != '\0')) { //B, and we're not in root directory
                    char* rightmostSlash = strrchr(currentPath, '/');
                    char* secondRightmostSlash = strchr(rightmostSlash, '/');
                    secondRightmostSlash[1] = '\0';

                    //free old directory
                    free(currentDirectoryContents);

                    //load new directory and reset menu state
                    currentlySelectedEntryIndex = 0;
                    currentDirectoryEntriesNum = GetDirectoryEntries(device, currentPath, &currentDirectoryContents);
                    if (currentDirectoryEntriesNum < 0) {
                        consoleClear();
                        printf("Failed to open '%s:%s'\n", device, currentPath);
                        consoleUpdate(console);
                        consoleExit(console);
                        return NULL;
                    }
                }
            }
        } else {
            padCooldown--;
        }

        //Draw top bar
        printf("%10s%s\n", "", "vita2hos " VITA2HOS_MAJOR "." VITA2HOS_MINOR "." VITA2HOS_PATCH "-" VITA2HOS_HASH " (" __DATE__ " " __TIME__ ")");
        printf("%5sCurrent directory: %s:%s\t%lld/%lld\n\n", "", device, currentPath, currentlySelectedEntryIndex+1, currentDirectoryEntriesNum);

        if (currentDirectoryEntriesNum != 0) {
            printf("  %-60s %-10s\n", "NAME", "SIZE");
            puts("----------------------------------------------------------------------");

            for (u64 i = 0; i < (u64)currentDirectoryEntriesNum; i++) {
                char leading = '|';
                if (i == currentlySelectedEntryIndex) {
                    leading = '>';
                }
                switch (currentDirectoryContents[i].type) {
                    case FsDirEntryType_File:
                        printf("%c %-60s %lld\n", leading, currentDirectoryContents[i].name, currentDirectoryContents[i].file_size);
                        break;

                    case FsDirEntryType_Dir:
                        printf("%c %-60s %-10s\n", leading, currentDirectoryContents[i].name, "Directory");
                        break;

                    default:
                        printf("%c %-60s %lld ?%hhx?\n", leading, currentDirectoryContents[i].name, currentDirectoryContents[i].file_size, currentDirectoryContents[i].type);
                        break;
                }
            }
        } else {
            puts("----------------------------------------------------------------------");
            puts("Empty directory");
        }

        consoleUpdate(console);
        svcSleepThread(1 * 1000 * 1000); //Sleep 1ms to avoid burning CPU

    }
    //TODO make this prettier
    memset(elfPath, 0, sizeof(elfPath));
    strncpy(elfPath, currentPath, sizeof(elfPath));
    size_t slashIdx = strnlen(elfPath, sizeof(elfPath));
    if (slashIdx >= (sizeof(elfPath)-1)) { //no room to append / and \0
        LOG("PATH TOO BIG!");
        while (1) { svcSleepThread(1000000000LL * 60LL); }
    }
    elfPath[slashIdx] = '/';
    elfPath[sizeof(elfPath) - 1] = '\0';
    strncat(elfPath, currentDirectoryContents[currentlySelectedEntryIndex].name, sizeof(elfPath)-1);
    elfPath[sizeof(elfPath) - 1] = '\0';

    free(currentDirectoryContents);
    consoleClear();
    consoleExit(console);

    return elfPath;
}