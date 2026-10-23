// Standalone Linux installation helper. No Qt or mounted AppImage dependencies.
// Downloads/signatures are verified by the client before this helper is started.
// The private staging directory and installer lock remain owned by this user.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
bool regularOwned(const std::string &path) {
    struct stat st{};
    return lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid();
}

bool writeFile(const std::string &path, const std::string &text) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const bool ok = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) && fsync(fd) == 0;
    close(fd);
    return ok;
}

std::string readFile(const std::string &path) {
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return {};
    char buffer[512];
    ssize_t count = read(fd, buffer, sizeof(buffer));
    close(fd);
    return count > 0 ? std::string(buffer, static_cast<size_t>(count)) : std::string();
}

bool exchange(const std::string &a, const std::string &b) {
    // RENAME_EXCHANGE keeps either the old or new complete image at the launch
    // path even if power is lost. Both files are on the same filesystem.
    return syscall(SYS_renameat2, AT_FDCWD, a.c_str(), AT_FDCWD, b.c_str(), 2) == 0;
}

void syncDir(const std::string &path) {
    int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) { fsync(fd); close(fd); }
}

pid_t launch(const std::string &target, const std::string &health = {}) {
    pid_t child = fork();
    if (child != 0) return child;
    setsid();
    if (health.empty()) execl(target.c_str(), target.c_str(), static_cast<char *>(nullptr));
    else execl(target.c_str(), target.c_str(), "--update-health-file", health.c_str(), static_cast<char *>(nullptr));
    _exit(127);
}
}

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "Usage: magnite-update-helper APPIMAGE STAGING PARENT_PID VERSION\n"); return 2; }
    const std::string target = argv[1], stage = argv[2], version = argv[4];
    char *end = nullptr;
    long parentValue = strtol(argv[3], &end, 10);
    if (!end || *end || parentValue <= 1) return 2;
    const pid_t parent = static_cast<pid_t>(parentValue);
    const std::string incoming = stage + "/incoming.AppImage", health = stage + "/healthy";
    const auto slash = target.find_last_of('/');
    if (slash == std::string::npos || stage.rfind(target.substr(0, slash + 1) + ".magnite-update-", 0) != 0
        || stage.find("/../") != std::string::npos || !regularOwned(target) || !regularOwned(incoming)) return 2;
    struct stat st{};
    if (lstat(stage.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) return 2;
    const std::string state = stage + "/status";
    int lock = open((target + ".update-lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0 || !writeFile(state, "ready")) return 2;
    // Client retains the lock through its entire lifetime. The helper does not
    // change files until both the client and its supervised backend are gone.
    bool acquired = false;
    for (int tick = 0; tick < 600; ++tick) {
        if (access((stage + "/cancel").c_str(), F_OK) == 0) return 3;
        if (kill(parent, 0) < 0 && errno == ESRCH && flock(lock, LOCK_EX | LOCK_NB) == 0) {
            acquired = true; break;
        }
        usleep(100000);
    }
    if (!acquired) { writeFile(state, "cancelled: application did not exit"); return 3; }
    if (access((stage + "/cancel").c_str(), F_OK) == 0) return 3;
    if (!regularOwned(target) || !regularOwned(incoming) || chmod(incoming.c_str(), 0755)
        || !exchange(target, incoming)) {
        writeFile(state, "failed: could not atomically install update");
        flock(lock, LOCK_UN); close(lock); launch(target); return 4;
    }
    syncDir(stage); syncDir(target.substr(0, slash));
    writeFile(state, "installed; checking startup");
    flock(lock, LOCK_UN);
    pid_t child = launch(target, health);
    bool healthy = false;
    for (int tick = 0; child > 0 && tick < 1200; ++tick) {
        if (readFile(health) == version) { healthy = true; break; }
        int status = 0;
        if (waitpid(child, &status, WNOHANG) == child) break;
        usleep(100000);
    }
    if (healthy) {
        // Retain old bytes in this transaction directory for manual recovery.
        rename(incoming.c_str(), (stage + "/previous.AppImage").c_str());
        writeFile(state, "success: " + version);
        syncDir(stage);
        close(lock);
        return 0;
    }
    // Stop the failed new process group before touching its installation.
    if (child > 0) {
        kill(-child, SIGTERM);
        for (int tick = 0; tick < 200 && kill(-child, 0) == 0; ++tick) usleep(100000);
        kill(-child, SIGKILL);
        waitpid(child, nullptr, 0);
    }
    // Never overwrite an image still used by another application instance.
    if (flock(lock, LOCK_EX | LOCK_NB) || !exchange(target, incoming)) {
        writeFile(state, "recovery required: previous AppImage is incoming.AppImage");
        return 5;
    }
    syncDir(stage); syncDir(target.substr(0, slash));
    writeFile(state, "rolled back: new version did not become ready");
    flock(lock, LOCK_UN); close(lock);
    launch(target);
    return 6;
}
