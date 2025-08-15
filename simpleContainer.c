#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <errno.h>

#define STACK_SIZE (1024 * 1024 * 4)
static char child_stack[STACK_SIZE];

// Check if directory exits or not
void dir_exist(const char *path, mode_t mode){
    if(mkdir(path, mode) == -1 && errno != EEXIST){
        perror(path);
        exit(EXIT_FAILURE);
    }
}

// Function to write a string value to a file
void write_to_file(const char *path, const char *value){
    FILE *f = fopen(path, "w");
    if(!f){
        perror(path);
        exit(EXIT_FAILURE);
    }
    if(fprintf(f, "%s", value) < 0){
        perror("fprintf");
        fclose(f);
        exit(EXIT_FAILURE);
    }
    fclose(f);
}

// ------------------------------ Child Function ------------------------------ //
int child_func(void *arg){
    int *pipefd = (int *)arg;
    close(pipefd[1]); // Closes write end

    printf("---------- Inside container (PID: %d) ----------\n", getpid());

    // Set hostname
    if(sethostname("prkContainer", 12) == -1){
        perror("sethostname");
        exit(EXIT_FAILURE);
    }

    // Making mount namespace private
    if(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == -1){
        perror("mount MS_PRIVATE");
        exit(EXIT_FAILURE);
    }

    // Waiting for parent to configure networking and cgroups
    char buffer;
    read(pipefd[0], &buffer, 1);
    close(pipefd[0]);

    // chroot into rootfs
    if(chroot("./rootfs") == -1){
        perror("chroot");
        exit(EXIT_FAILURE);
    }
    if(chdir("/") == -1){
        perror("chdir");
        exit(EXIT_FAILURE);
    }

    // Ensuring /proc, /sys, and /dev exit
    dir_exist("/proc", 0555);
    dir_exist("/sys", 0555);
    dir_exist("/dev", 0555);
    // Mounting /proc, /sys and /dev
    if(mount("proc", "/proc", "proc", 0, NULL) == -1){
        perror("mount /proc");
        exit(EXIT_FAILURE);
    }
    if(mount("sysfs", "/sys", "sysfs", 0, NULL) == -1){
        perror("mount /sys");
    }
    if(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == -1){
        perror("mount /dev");
    }
    // Show network interfaces
    printf("\n ---------- Network interfaces inside the container ----------\n");
    system("ip a");

    // Launching BusyBox shell
    char *const args[] = {"/bin/sh", NULL};
    execv(args[0], args);
    perror("execv /bin/sh");
    return 1;
}

// ------------------------------ Main Function ------------------------------ //
int main(){
    printf("Starting container...\n");

    // IPC b/w parent and child
    int pipefd[2];
    if(pipe(pipefd) == -1){
        perror("pipe");
        exit(EXIT_FAILURE);
    }
    // Creating a new child process with new PID, mount, and network namespaces
    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWUTS | SIGCHLD;
    pid_t child_pid = clone(child_func, child_stack + STACK_SIZE, flags, pipefd);
    if(child_pid == -1){
        perror("clone");
        exit(EXIT_FAILURE);
    }

    // ------------------------------ Parent process ------------------------------ //

    close(pipefd[0]); // Closes read end
    sleep(1); // To give child time to enter namespace

    // ------------------------------ Setting up cgroup v2 ------------------------------ //

    const char *cgroup_base = "/sys/fs/cgroup";
    const char *cgroup_name = "prkcontainer";
    char cgroup_path[256];

    snprintf(cgroup_path, sizeof(cgroup_path), "%s/%s", cgroup_base, cgroup_name);

    // Creating cgroup directory
    if(mkdir(cgroup_path, 0755) == -1 && errno != EEXIST){
        perror("mkdir cgroup");
        exit(EXIT_FAILURE);
    }

    // Limiting max processes to 10
    char path[512];
    snprintf(path, sizeof(path), "%s/pids.max", cgroup_path);
    write_to_file(path, "10");

    // Limiting memory usage to 100 MB (100*1024*1024)
    snprintf(path, sizeof(path), "%s/memory.max", cgroup_path);
    write_to_file(path, "104857600");

    // Limiting CPU usage: 50% of 1 CPU
    snprintf(path, sizeof(path), "%s/cpu.max", cgroup_path);
    write_to_file(path, "50000 100000");     // To limit to 50%, quota=50000, period=100000

    // Adding child process to this cgroup
    snprintf(path, sizeof(path), "%s/cgroup.procs", cgroup_path);
    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d", child_pid);
    write_to_file(path, pid_str);

    // ------------------------------ Setting up networking ------------------------------ //

    // Setting up veth pair
    system("ip link add veth-host type veth peer name veth-container");

    // Moving one end into container's net namespace
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ip link set veth-container netns %d", child_pid);
    system(cmd);

    // Configure host side
    system("ip addr add 192.168.1.1/24 dev veth-host");
    system("ip link set veth-host up");

    // Creating netns symlink for ip netns exec
    system("mkdir -p /var/run/netns");
    snprintf(cmd, sizeof(cmd), "ln -sf /proc/%d/ns/net /var/run/netns/prknetns", child_pid);
    system(cmd);

    // Configure container side
    snprintf(cmd, sizeof(cmd),
        "ip netns exec prknetns ip addr add 192.168.1.2/24 dev veth-container && "
        "ip netns exec prknetns ip link set veth-container up && "
        "ip netns exec prknetns ip link set lo up && "
        "ip netns exec prknetns ip route add default via 192.168.1.1"
    );
    system(cmd);

    // Enable NAT
    system("sysctl -w net.ipv4.ip_forward=1 > /dev/null");
    system("iptables -t nat -A POSTROUTING -s 192.168.1.0/24 -j MASQUERADE");

    // Signal child to continue
    write(pipefd[1], "1", 1);
    close(pipefd[1]);

    // Waiting for child to exit
    waitpid(child_pid, NULL, 0);

    // Cleanup netns symlink
    system("rm -f /var/run/netns/prknetns");

    // Remove cgroup directory
    if(rmdir(cgroup_path) == -1){
        perror("rmdir cgroup");
    }

    printf("Container exited.\n");
    return 0;
}