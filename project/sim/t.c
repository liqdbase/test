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
    uint32_t  id;           // zone 번호 (0부터)
    uint64_t  start_lba;    // zone 시작 LBA
    uint64_t  end_lba;      // zone 마지막 LBA (inclusive)
    uint64_t  wp;           // write pointer (다음에 쓸 LBA)
} zns_zone_t;

static zns_zone_t *zones = NULL;
static uint32_t    zone_count = 0;
static uint8_t    *storage = NULL;    // 전체 디바이스 메모리
static FILE       *log_file = NULL;   // 로그 파일

// 초기화: zone_size_mb 인자로 받아 zone 분할
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

    // 1GiB 메모리 할당
    storage = malloc(TOTAL_CAPACITY);
    if (!storage) {
        perror("storage malloc");
        exit(1);
    }
    memset(storage, 0xff, TOTAL_CAPACITY);  // 초기화 (optional)
}

// 로그 파일 열기
void open_log_file(const char *filename) {
    log_file = fopen(filename, "w");
    if (!log_file) {
        perror("로그 파일 열기 실패");
        exit(1);
    }
    fprintf(log_file, "# LBA ACTION\n");
    fprintf(log_file, "# ACTION: W=write, R=read\n");
}

// 로그 파일 닫기
void close_log_file() {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

// zone에 순차 쓰기
//   zone_id   : 0 ~ zone_count-1
//   lba_cnt   : LBA 개수
//   pattern   : 저장할 바이트 패턴 (테스트용)
int write_zone(uint32_t zone_id, uint64_t lba_cnt, uint8_t pattern) {
    if (zone_id >= zone_count) {
        fprintf(stderr, "Invalid zone_id %u\n", zone_id);
        return -1;
    }
    zns_zone_t *z = &zones[zone_id];

    // 쓰기 가능 범위 체크
    if (z->wp + lba_cnt - 1 > z->end_lba) {
        fprintf(stderr, "Zone %u overflow: wp=%llu + %llu > %llu\n",
                zone_id,
                (unsigned long long)z->wp,
                (unsigned long long)lba_cnt,
                (unsigned long long)z->end_lba);
        return -1;
    }

    // 메모리 상에 순차 쓰기
    uint64_t byte_offset = z->wp * LBA_SIZE;
    uint64_t byte_len    = lba_cnt * LBA_SIZE;
    memset(storage + byte_offset, pattern, byte_len);

    // 로그 기록
    if (log_file) {
        for (uint64_t i = 0; i < lba_cnt; i++) {
            fprintf(log_file, "%llu W\n", (unsigned long long)(z->wp + i));
        }
    }

    z->wp += lba_cnt;  // write pointer 진전
    return 0;
}

// zone에서 랜덤 읽기
//   zone_id   : 0 ~ zone_count-1
//   lba_cnt   : 읽을 LBA 개수
//   outbuf    : 버퍼 (lba_cnt * LBA_SIZE 바이트 이상)
// 반환값: 0=성공, -1=오류, 성공 시 시작 LBA 반환
int64_t read_zone_random(uint32_t zone_id, uint64_t lba_cnt, uint8_t *outbuf) {
    if (zone_id >= zone_count) {
        fprintf(stderr, "Invalid zone_id %u\n", zone_id);
        return -1;
    }
    zns_zone_t *z = &zones[zone_id];

    // 랜덤 시작점: [start_lba, wp - lba_cnt]
    if (z->wp - z->start_lba < lba_cnt) {
        fprintf(stderr, "Zone %u: 아직 %llu LBAs가 채워지지 않았습니다.\n",
                zone_id,
                (unsigned long long)lba_cnt);
        return -1;
    }
    uint64_t max_off = (z->wp - lba_cnt) - z->start_lba;
    uint64_t rel_lba = (uint64_t)rand() % (max_off + 1);
    uint64_t abs_lba = z->start_lba + rel_lba;

    uint64_t byte_offset = abs_lba * LBA_SIZE;
    uint64_t byte_len    = lba_cnt * LBA_SIZE;
    memcpy(outbuf, storage + byte_offset, byte_len);

    // 로그 기록
    if (log_file) {
        for (uint64_t i = 0; i < lba_cnt; i++) {
            fprintf(log_file, "%llu R\n", (unsigned long long)(abs_lba + i));
        }
    }

    return abs_lba;
}

// 존 리셋 - 해당 존의 write pointer를 시작점으로 되돌림
int reset_zone(uint32_t zone_id) {
    if (zone_id >= zone_count) {
        fprintf(stderr, "Invalid zone_id %u\n", zone_id);
        return -1;
    }
    
    zones[zone_id].wp = zones[zone_id].start_lba;
    return 0;
}

// 워크로드 생성 함수
void generate_workload(uint32_t num_requests, double write_ratio) {
    uint8_t *buffer = malloc(1024 * LBA_SIZE); // 최대 1024 LBA 버퍼
    if (!buffer) {
        perror("버퍼 할당 실패");
        exit(1);
    }

    printf("워크로드 생성 시작: %u 요청, 쓰기 비율 %.2f\n", 
           num_requests, write_ratio);

    for (uint32_t i = 0; i < num_requests; i++) {
        // 랜덤 존 선택
        uint32_t zone_id = rand() % zone_count;
        
        // 랜덤 LBA 개수 (1~64)
        uint32_t lba_cnt = 1 + (rand() % 64);
        
        // 쓰기 또는 읽기 결정
        double r = (double)rand() / RAND_MAX;
        
        if (r < write_ratio) {
            // 쓰기 작업
            if (zones[zone_id].wp + lba_cnt > zones[zone_id].end_lba) {
                // 존이 가득 차면 리셋
                reset_zone(zone_id);
                printf("Zone %u reset\n", zone_id);
            }
            
            if (write_zone(zone_id, lba_cnt, (uint8_t)rand()) == 0) {
                if (i % 1000 == 0) {
                    printf("요청 %u: Zone %u에 %u LBAs 쓰기 완료\n", 
                           i, zone_id, lba_cnt);
                }
            }
        } else {
            // 읽기 작업
            int64_t start_lba = read_zone_random(zone_id, lba_cnt, buffer);
            if (start_lba >= 0 && i % 1000 == 0) {
                printf("요청 %u: Zone %u에서 LBA %lld부터 %u LBAs 읽기 완료\n", 
                       i, zone_id, (long long)start_lba, lba_cnt);
            }
        }
        
        // 진행 상황 표시
        if (i % 10000 == 0 && i > 0) {
            printf("진행률: %.1f%% (%u/%u)\n", 
                   100.0 * i / num_requests, i, num_requests);
        }
    }
    
    free(buffer);
    printf("워크로드 생성 완료: %u 요청\n", num_requests);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <zone_size_MB> <num_requests> [write_ratio] [log_file]\n", argv[0]);
        fprintf(stderr, "  zone_size_MB: 존 크기 (MB 단위)\n");
        fprintf(stderr, "  num_requests: 생성할 요청 수\n");
        fprintf(stderr, "  write_ratio: 쓰기 비율 (0.0~1.0, 기본값 0.5)\n");
        fprintf(stderr, "  log_file: 로그 파일 이름 (기본값 zns_trace.log)\n");
        return 1;
    }
    
    uint32_t zone_mb = atoi(argv[1]);
    uint32_t num_requests = atoi(argv[2]);
    double write_ratio = 0.5;  // 기본값
    const char *log_filename = "zns_trace.log";
    
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

    printf("ZNS 시뮬레이터 초기화: 총용량=1GiB, 존크기=%uMB\n", zone_mb);
    init_zns(zone_mb);
    printf("  => 존 개수 = %u\n\n", zone_count);
    
    // 로그 파일 열기
    open_log_file(log_filename);
    
    // 워크로드 생성
    generate_workload(num_requests, write_ratio);
    
    // 로그 파일 닫기
    close_log_file();
    
    // 정리
    free(storage);
    free(zones);
    return 0;
}

