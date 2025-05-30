#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

#define TOTAL_CAPACITY   (1ULL << 30)  // 1 GiB
#define LBA_SIZE         512           // 512 bytes per LBA
#define TOTAL_LBAS       (TOTAL_CAPACITY / LBA_SIZE)

typedef struct {
    uint32_t  id;
    uint64_t  start_lba;
    uint64_t  end_lba;
    uint64_t  wp;
} zns_zone_t;

static zns_zone_t *zones = NULL;
static uint32_t    zone_count = 0;
static uint8_t    *storage = NULL;
static FILE       *log_file = NULL;

void init_zns(uint32_t zone_size_mb) {
    uint64_t zone_bytes = (uint64_t)zone_size_mb * 1024 * 1024;
    uint64_t zone_lbas  = zone_bytes / LBA_SIZE;

    if (zone_bytes == 0 || (TOTAL_CAPACITY % zone_bytes) != 0) {
        fprintf(stderr, "존 크기 오류: 1GiB은 %u MB 단위로만 나누어져야 합니다.\n", zone_size_mb);
        exit(1);
    }

    zone_count = TOTAL_LBAS / zone_lbas;
    zones = calloc(zone_count, sizeof(zns_zone_t));
    if (!zones) {
        perror("zones calloc");
        exit(1);
    }

    for (uint32_t i = 0; i < zone_count; i++) {
        zones[i].id        = i;
        zones[i].start_lba = i * zone_lbas;
        zones[i].end_lba   = (i+1) * zone_lbas - 1;
        zones[i].wp        = zones[i].start_lba;
    }

    storage = malloc(TOTAL_CAPACITY);
    if (!storage) {
        perror("storage malloc");
        exit(1);
    }
    memset(storage, 0xff, TOTAL_CAPACITY);
}

void open_log_file(const char *filename) {
    log_file = fopen(filename, "w");
    if (!log_file) {
        perror("로그 파일 열기 실패");
        exit(1);
    }
    fprintf(log_file, "# LBA ACTION\n");
    fprintf(log_file, "# ACTION: W=write, R=read\n");
}

void close_log_file() {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

void generate_mixed_workload(uint32_t num_requests, double write_ratio) {
    printf("혼합 워크로드 생성 시작 (%u 요청, 쓰기비율 %.2f)\n", num_requests, write_ratio);

    const int hot_set_size = 100;
    uint64_t hot_lbas[hot_set_size];
    for (int i = 0; i < hot_set_size; i++) {
        hot_lbas[i] = rand() % TOTAL_LBAS;
    }

    for (uint32_t i = 0; i < num_requests; i++) {
        double pattern_type = (double)rand() / RAND_MAX;
        uint64_t lba;

        if (pattern_type < 0.25) {
            lba = rand() % TOTAL_LBAS;  // 완전 랜덤
        } else if (pattern_type < 0.5) {
            lba = (i * 64) % TOTAL_LBAS;  // 순차
        } else if (pattern_type < 0.75) {
            lba = hot_lbas[rand() % hot_set_size];  // 재사용 중심
        } else {
            lba = rand() % TOTAL_LBAS;
            if (i % 100 == 0) {
                for (int j = 0; j < 3; j++) fprintf(log_file, "# idle\n");  // burst
            }
        }

        char op = ((double)rand() / RAND_MAX) < write_ratio ? 'W' : 'R';
        fprintf(log_file, "%llu %c\n", (unsigned long long)lba, op);
    }

    printf("혼합 워크로드 생성 완료\n");
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <zone_size_MB> <num_requests> [write_ratio] [log_file]\n", argv[0]);
        return 1;
    }

    uint32_t zone_mb = atoi(argv[1]);
    uint32_t num_requests = atoi(argv[2]);
    double write_ratio = 0.5;
    const char *log_filename = "zns_trace_mixed.log";

    if (argc > 3) {
        write_ratio = atof(argv[3]);
        if (write_ratio < 0.0 || write_ratio > 1.0) {
            fprintf(stderr, "쓰기 비율은 0.0~1.0 사이여야 합니다.\n");
            return 1;
        }
    }
    if (argc > 4) {
        log_filename = argv[4];
    }

    srand(time(NULL));
    init_zns(zone_mb);
    open_log_file(log_filename);
    generate_mixed_workload(num_requests, write_ratio);
    close_log_file();

    free(storage);
    free(zones);
    return 0;
}
