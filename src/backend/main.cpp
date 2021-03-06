#include "common/config.hpp"
#include "common/command.hpp"
#include "common/comm_queue.hpp"
#include "modules/module_manager.hpp"

#include <queue>
#include <future>
#include <sched.h>
#include <unistd.h>
#include <signal.h>
#include <sys/file.h>

#define __DEBUG
#include "common/debug.hpp"

static const unsigned int DEFAULT_PARALLELISM = 64;
static const std::string FILE_LOCK = "/dev/shm/veloc-lock-" + std::to_string(getuid());

bool ec_active = false;
int lock_fd = -1;

void exit_handler(int signum) {
    if (ec_active)
        MPI_Finalize();
    close(lock_fd);
    remove(FILE_LOCK.c_str());
    backend_cleanup();
    exit(signum);
}

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 3) {
	std::cout << "Usage: " << argv[0] << " <veloc_config> [--disable-ec]" << std::endl;
	return 1;
    }

    config_t cfg(argv[1]);
    if (cfg.is_sync()) {
	ERROR("configuration requests sync mode, backend is not needed");
	return 2;
    }

    // check if an instance is already running
    int lock_fd = open(FILE_LOCK.c_str(), O_RDWR | O_CREAT, 0644);
    if (lock_fd == -1) {
        ERROR("cannot open " << FILE_LOCK << ", error = " << strerror(errno));
        return 3;
    }
    int locked = flock(lock_fd, LOCK_EX | LOCK_NB);
    if (locked == -1) {
        if (errno == EWOULDBLOCK)
            INFO("backend already running, only one instance is needed");
        else
            ERROR("cannot acquire file lock: " << FILE_LOCK << ", error = " << strerror(errno));
        return 4;
    }

    // disable EC on request
    if (argc == 3 && std::string(argv[2]) == "--disable-ec") {
	INFO("EC module disabled by commmand line switch");
	ec_active = false;
    }

    // initialize MPI
    if (ec_active) {
	int rank;
	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	DBG("Active backend rank = " << rank);
    }

    int max_parallelism;
    if (!cfg.get_optional("max_parallelism", max_parallelism))
        max_parallelism = DEFAULT_PARALLELISM;
    INFO("Max number of client requests processed in parallel: " << max_parallelism);
    cpu_set_t cpu_mask;
    CPU_ZERO(&cpu_mask);
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    for (int i = 0; i < nproc; i++)
        CPU_SET(i, &cpu_mask);

    backend_cleanup();
    backend_t<command_t> command_queue;
    module_manager_t modules;
    modules.add_default_modules(cfg, MPI_COMM_WORLD, ec_active);

    // install handler to perform clean up on SIGTERM and SIGINT
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = exit_handler;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    std::queue<std::future<void> > work_queue;
    command_t c;
    while (true) {
	auto f = command_queue.dequeue_any(c);
	work_queue.push(std::async(std::launch::async, [=, &modules] {
                    // lower worker thread priority and set its affinity to whole CPUSET
                    sched_setaffinity(0, sizeof(cpu_set_t), &cpu_mask);
                    nice(10);
		    f(modules.notify_command(c));
		}));
	if (work_queue.size() > (unsigned int)max_parallelism) {
	    work_queue.front().wait();
	    work_queue.pop();
	}
    }

    return 0;
}
