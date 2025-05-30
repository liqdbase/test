#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> // For strcasecmp
#include <ctype.h>   // For tolower
#include <limits.h>  // For ULLONG_MAX, UINT_MAX
#include <errno.h>   // For errno and strerror

// --- 기본 설정 ---
#define MAX_BUFFER_SIZE 65000     // 버퍼의 최대 크기 (필요시 조정)
#define MAX_FILENAME_LEN 256      // 로그 파일 이름 최대 길이
#define DEVICE_NAME "/dev/nvme0n1" // 로그용 장치 이름

// --- 페이지/블록 관련 설정 ---
#define SECTOR_SIZE 512           // 표준 섹터 크기 (바이트)
#define SECTORS_PER_PAGE 8        // 페이지당 섹터 수 (예: 4KB 페이지 / 512B 섹터 LBA)
#define INVALID_PAGE ULLONG_MAX   // 유효하지 않은 페이지 ID

// Operation Types
#define OP_READ 0
#define OP_WRITE 1

// --- ZNS 관련 설정 ---
#define INVALID_ZONE ULLONG_MAX
#define MAX_ZONES 131072 // 최대 Zone 개수 가정 (예: 512GB / 4MB Zone), 필요시 조정

// --- 교체 정책 정의 ---
typedef enum {
    CLOCK_PRO_T1_B4_LOGS_B2 = 0, // 7. T1캐시, B4히스토리, B2로그 -> 값 변경
    CLOCK_PRO_T3_B2_LOGS_B4 = 1, // 8. T3캐시, B2히스토리, B4로그 -> 값 변경
    CLOCK_T1 = 2,                // 5. T1을 사용하는 기본 CLOCK 정책 -> 값 변경
    CLOCK_T3 = 3,                // 6. T3을 사용하는 기본 CLOCK 정책 -> 값 변경
    FIFO = 4,                    // 0. 기본값 -> 값 변경
    LFU = 5,                     // 2. T3(전체버퍼)을 LFU 캐시로 사용... -> 값 변경
    LFU_ARC = 6,                 // 4. T3/T4 기반 원래 ARC -> 값 변경
    LRU = 7,                     // 1. T1(전체버퍼)을 LRU 캐시로 사용... -> 값 변경
    LRU_ARC = 8                  // 3. T1/T2 기반 원래 ARC -> 값 변경
} ReplacementPolicy;

const char* policy_names[] = {
    "CLOCK_PRO_T1_B4_LOGS_B2", // Index 0
    "CLOCK_PRO_T3_B2_LOGS_B4", // Index 1
    "CLOCK_T1",                // Index 2
    "CLOCK_T3",                // Index 3
    "FIFO",                    // Index 4
    "LFU",                     // Index 5
    "LFU_ARC",                 // Index 6
    "LRU",                     // Index 7
    "LRU_ARC"                  // Index 8
};

// --- 버퍼 프레임 구조체 ---
typedef struct {
    unsigned long long page_id;
    unsigned long long load_time;       // 페이지 로드 시간 (FIFO, LFU tie-break)
    unsigned long long last_access_time;// 마지막 접근 시간 (LRU계열 정책용)
    unsigned int access_count;          // 접근 빈도 (LFU계열 정책용)
    int list_type; // 현재 정책에 따른 실제 리스트 타입
                   // LRU: 1 (T1_ref 개념) -> LRU는 이제 7, list_type 1은 여전히 T1 참조용으로 사용될 수 있음
                   // LFU: 3 (T3_ref 개념) -> LFU는 이제 5, list_type 3은 여전히 T3 참조용으로 사용될 수 있음
                   // LRU_ARC: 1(T1), 2(T2) -> LRU_ARC는 이제 8
                   // LFU_ARC: 3(T3), 4(T4) -> LFU_ARC는 이제 6
                   // CLOCK_T1, CLOCK_PRO_T1...: 1 (T1 캐시) -> CLOCK_T1은 2, CLOCK_PRO_T1...은 0
                   // CLOCK_T3, CLOCK_PRO_T3...: 3 (T3 캐시) -> CLOCK_T3은 3, CLOCK_PRO_T3...은 1
    int is_dirty;  // Dirty flag (0: clean, 1: dirty)
    int ref_arc_list_type; // LRU/LFU 실행 중, 참조용 ARC 시뮬레이션에서의 리스트 타입
                           // LRU policy: 1 (T1_ref), 2 (T2_ref)
                           // LFU policy: 3 (T3_ref), 4 (T4_ref)
                           // 0 if not applicable or page is invalid
    int ref_bit;           // CLOCK 알고리즘용 참조 비트 (0 또는 1)
} BufferFrame;

// --- ARC 상태 구조체 ---
typedef struct {
    // For T1/T2 (LRU_ARC) and T1 (LRU policy reference)
    int p;        // T1 target size (LRU_ARC 에서만 동적, LRU에서는 참조용 p, CLOCK_PRO_T1... 에서 T1 목표 크기)
    int t1_size;  // LRU_ARC: 실제 T1 크기, LRU: 참조용 T1_ref 크기, CLOCK_T1/PRO_T1: 실제 T1 캐시 크기
    int t2_size;  // LRU_ARC: 실제 T2 크기, LRU: 참조용 T2_ref 크기, CLOCK_PRO_T1_B4_LOGS_B2: B2 로그 크기
    int b1_size;
    unsigned long long b1[MAX_BUFFER_SIZE]; // Ghost list for T1 / T1_ref
    int b2_size;
    unsigned long long b2[MAX_BUFFER_SIZE]; // LRU_ARC의 B2, CLOCK_PRO_T3_B2_LOGS_B4의 히스토리(B2_hist), CLOCK_PRO_T1_B4_LOGS_B2의 로그(B2_log)

    // For T3/T4 (LFU_ARC) and T3 (LFU policy reference)
    int q;        // T3 target size (LFU_ARC 에서만 동적, LFU에서는 참조용 q, CLOCK_PRO_T3... 에서 T3 목표 크기)
    int t3_size;  // LFU_ARC: 실제 T3 크기, LFU: 참조용 T3_ref 크기, CLOCK_T3/PRO_T3: 실제 T3 캐시 크기
    int t4_size;  // LFU_ARC: 실제 T4 크기, LFU: 참조용 T4_ref 크기, CLOCK_PRO_T3_B2_LOGS_B4: B4 로그 크기
    int b3_size;
    unsigned long long b3[MAX_BUFFER_SIZE]; // Ghost list for T3 / T3_ref
    int b4_size;
    unsigned long long b4[MAX_BUFFER_SIZE]; // LFU_ARC의 B4, CLOCK_PRO_T1_B4_LOGS_B2의 히스토리(B4_hist), CLOCK_PRO_T3_B2_LOGS_B4의 로그(B4_log)

    // CLOCK 정책 및 CLOCK-Pro 정책용 핸드
    int p_clk_hand; // CLOCK_T1 또는 CLOCK_PRO_T1 계열 정책의 T1 파티션 핸드
    int q_clk_hand; // CLOCK_T3 또는 CLOCK_PRO_T3 계열 정책의 T3 파티션 핸드
} ARCState;

// --- 전역 변수 및 상태 ---
BufferFrame buffer[MAX_BUFFER_SIZE];
int buffer_size = 0;
ReplacementPolicy current_policy;
ReplacementPolicy previous_policy_for_state_carryover; // 정책 변경 시 상태 이전 결정용
unsigned long long current_time = 0;
long long hits = 0;
long long misses = 0;
ARCState arc_state; // LRU/LFU일때도 참조용으로 사용됨, CLOCK_PRO 계열에서도 사용
FILE *log_file = NULL;
int global_clk_hand = 0; // 단순 CLOCK_T1, CLOCK_T3 정책용 (ARCState 핸드와 구분될 때)
unsigned long long zone_size_pages_global = 0; // 전역 존 크기 (페이지 단위)
unsigned long long zone_write_pointers[MAX_ZONES]; // 각 Zone의 현재 쓰기 포인터 (페이지 ID)

// Utility function MAX and MIN
#ifndef MAX
#define MAX(a,b) (((a)>(b))?(a):(b))
#endif
#ifndef MIN
#define MIN(a,b) (((a)<(b))?(a):(b))
#endif

// --- 함수 프로토타입 ---
void initialize_buffer();
void initialize_arc_state(int full_reset);
void initialize_zone_write_pointers(); // Zone 쓰기 포인터 초기화 함수
int find_in_buffer(unsigned long long page_id);
int find_empty_slot();
int evict_fifo();
void arc_remove_from_ghost(unsigned long long page_id, unsigned long long* list, int* list_size);
void arc_add_to_ghost_mru(unsigned long long page_id, unsigned long long* list, int* list_size, int max_ghost_size);
int find_in_arc_ghost(unsigned long long page_id, const unsigned long long* list, int list_size);
void handle_dirty_eviction(int victim_idx);
void write_fio_log(unsigned long long start_lba, unsigned int num_sectors, int operation_type); // ZNS 순차 쓰기 검사 추가
unsigned long long lba_to_page_id(unsigned long long lba);
int evict_arc_internal_lru(int target_list_type_val);
int evict_arc_internal_lfu(int target_list_type_val);
int arc_find_victim_lru_arc(unsigned long long page_id_to_load);
int arc_find_victim_lfu_arc(unsigned long long page_id_to_load);
int evict_via_clock_policy(int *hand_ptr, int list_type_filter_active, int target_list_type, BufferFrame* buffer_frames, int current_buffer_size, const char* policy_name_for_log);
void access_page(unsigned long long lba_address, unsigned long long page_id, int operation_type);
// void print_buffer_state(); // 주석 처리


// --- 함수 구현 ---

unsigned long long lba_to_page_id(unsigned long long lba) {
    if (SECTORS_PER_PAGE == 0) {
         fprintf(stderr, "Error: SECTORS_PER_PAGE cannot be zero.\n");
         exit(EXIT_FAILURE);
    }
    return lba / SECTORS_PER_PAGE;
}

void initialize_buffer() {
    for (int i = 0; i < buffer_size; i++) {
        buffer[i].page_id = INVALID_PAGE;
        buffer[i].load_time = 0;
        buffer[i].last_access_time = 0;
        buffer[i].access_count = 0;
        buffer[i].list_type = 0;
        buffer[i].is_dirty = 0;
        buffer[i].ref_arc_list_type = 0;
        buffer[i].ref_bit = 0;
    }
    hits = 0;
    misses = 0;
    current_time = 0;
    // previous_policy_for_state_carryover = FIFO; // FIFO는 이제 4
    // 초기 정책 설정은 main에서 하므로, 여기서 특정 값으로 고정할 필요는 없음.
    // main에서 current_policy가 설정된 후 previous_policy_for_state_carryover = current_policy;로 설정됨.
    global_clk_hand = 0;
}

// Zone 쓰기 포인터 초기화 함수
void initialize_zone_write_pointers() {
    if (zone_size_pages_global == 0) {
        //fprintf(stderr, "Warning: Zone size is 0, ZNS constraints disabled.\n");
        return; // Zone 크기가 0이면 ZNS 기능을 사용하지 않음
    }
    // 모든 Zone의 시작 페이지 ID로 쓰기 포인터 초기화
    // 실제로는 Zone 상태(Empty, Open 등)에 따라 달라져야 함
    // 여기서는 단순화를 위해 모든 Zone이 비어있고 쓰기 가능하다고 가정
    for (int i = 0; i < MAX_ZONES; i++) {
        zone_write_pointers[i] = (unsigned long long)i * zone_size_pages_global;
    }
    // printf("Initialized %d zone write pointers.\n", MAX_ZONES); // 디버깅용
}


// ARC 상태 초기화 함수
void initialize_arc_state(int full_reset) {
    if (full_reset) {
        arc_state.p = 0; arc_state.t1_size = 0; arc_state.t2_size = 0;
        arc_state.b1_size = 0; arc_state.b2_size = 0;
        for(int i=0; i<MAX_BUFFER_SIZE; ++i) {
            arc_state.b1[i] = INVALID_PAGE; arc_state.b2[i] = INVALID_PAGE;
        }

        arc_state.q = 0; arc_state.t3_size = 0; arc_state.t4_size = 0;
        arc_state.b3_size = 0; arc_state.b4_size = 0;
        for(int i=0; i<MAX_BUFFER_SIZE; ++i) {
            arc_state.b3[i] = INVALID_PAGE; arc_state.b4[i] = INVALID_PAGE;
        }

        arc_state.p_clk_hand = 0;
        arc_state.q_clk_hand = 0;

        // 정책별 특화 초기화 (enum 심볼을 사용하므로 값 변경에 자동 대응)
        if (current_policy == CLOCK_T1) { // CLOCK_T1은 이제 2
            arc_state.p = (buffer_size > 0) ? buffer_size : 0;
            arc_state.t1_size = 0;
        } else if (current_policy == CLOCK_T3) { // CLOCK_T3은 이제 3
            arc_state.q = (buffer_size > 0) ? buffer_size : 0;
            arc_state.t3_size = 0;
        } else if (current_policy == CLOCK_PRO_T1_B4_LOGS_B2) { // CLOCK_PRO_T1_B4_LOGS_B2는 이제 0
            arc_state.p = (buffer_size > 0) ? buffer_size / 2 : 0;
            arc_state.t1_size = 0;
        } else if (current_policy == CLOCK_PRO_T3_B2_LOGS_B4) { // CLOCK_PRO_T3_B2_LOGS_B4는 이제 1
            arc_state.q = (buffer_size > 0) ? buffer_size / 2 : 0;
            arc_state.t3_size = 0;
        }
    } else {
        // 상태 이어받기 (기존 로직 유지)
    }
}

int find_in_buffer(unsigned long long page_id) {
    for (int i = 0; i < buffer_size; i++) {
        if (buffer[i].page_id == page_id) return i;
    }
    return -1;
}

int find_empty_slot() {
    for (int i = 0; i < buffer_size; i++) {
        if (buffer[i].page_id == INVALID_PAGE) return i;
    }
    return -1;
}

// FIO 로그 작성 함수 (ZNS 순차 쓰기 제약 검사 및 적용)
void write_fio_log(unsigned long long start_lba, unsigned int num_sectors, int operation_type) {
    if (log_file == NULL || SECTOR_SIZE <= 0 || num_sectors == 0) return;

    unsigned long long offset_bytes = start_lba * SECTOR_SIZE;
    unsigned long long length_bytes_ull = (unsigned long long)num_sectors * SECTOR_SIZE;
    unsigned int length_bytes = (length_bytes_ull > UINT_MAX) ? UINT_MAX : (unsigned int)length_bytes_ull;
    if (length_bytes_ull > UINT_MAX) fprintf(stderr, "Warning: I/O length overflow LBA %llu.\n", start_lba);

    const char *action_str = (operation_type == OP_READ) ? "read" : "write";

    // ZNS 순차 쓰기 제약 검사 (쓰기 작업이고, Zone 크기가 설정된 경우)
    if (operation_type == OP_WRITE && zone_size_pages_global > 0) {
        unsigned long long target_page_id = lba_to_page_id(start_lba);
        unsigned long long zone_id = target_page_id / zone_size_pages_global;
        unsigned long long zone_start_page = zone_id * zone_size_pages_global;
        // num_pages 계산 (올림 처리)
        unsigned int num_pages = (num_sectors + SECTORS_PER_PAGE - 1) / SECTORS_PER_PAGE;
        if (num_pages == 0 && num_sectors > 0) num_pages = 1; // 최소 1 페이지

        // Zone ID 유효성 검사
        if (zone_id >= MAX_ZONES) {
            fprintf(stderr, "ZNS Error: Target Zone ID %llu exceeds MAX_ZONES %d for Page %llu. Write skipped.\n",
                    zone_id, MAX_ZONES, target_page_id);
            return; // 로그 기록 안 함
        }

        // 현재 Zone의 쓰기 포인터 가져오기
        unsigned long long current_wp = zone_write_pointers[zone_id];

        // 순차 쓰기 검사
        if (target_page_id != current_wp) {
            fprintf(stderr, "ZNS Violation: Non-sequential write attempt on Zone %llu. Target Page: %llu, Expected WP: %llu. Logging write anyway.\n",
                    zone_id, target_page_id, current_wp);
            // 실제 ZNS에서는 이 쓰기가 실패해야 하지만, 시뮬레이션에서는 로그는 남김
        } else {
            // 순차 쓰기 성공. 쓰기 포인터 업데이트
            // 주의: 이 업데이트는 쓰기가 Zone 경계를 넘지 않는다고 가정함.
            // 실제 ZNS에서는 Zone 용량 체크 및 경계 처리 필요.
            unsigned long long next_wp = current_wp + num_pages;
            unsigned long long zone_end_page = zone_start_page + zone_size_pages_global;
            if (next_wp > zone_end_page) {
                 fprintf(stderr, "ZNS Warning: Write attempt spans across Zone %llu boundary (Target: %llu, End: %llu). Adjusting WP to zone end. Logging write anyway.\n",
                         zone_id, next_wp, zone_end_page);
                 zone_write_pointers[zone_id] = zone_end_page; // 실제로는 Zone Full 상태 처리 필요
            } else {
                 zone_write_pointers[zone_id] = next_wp;
                 // printf("ZNS Info: Sequential write success on Zone %llu. New WP: %llu\n", zone_id, next_wp); // 디버깅용
            }
        }
        // --- 추가 ZNS 제약 조건 검사 위치 ---
        // 1. Zone 상태 확인 (Open/Empty 인가?) - 이 시뮬레이터는 상태 추적 안 함
        // 2. Zone 용량 확인 (쓰려는 크기가 남은 공간보다 작은가?) - 위에서 간단히 경계 체크로 대체
        // ------------------------------------
    }

    // FIO 로그 기록 (순차성 위반 여부와 관계없이 기록 - 시뮬레이션 흐름 유지)
    fprintf(log_file, "%s %s %llu %u\n", DEVICE_NAME, action_str, offset_bytes, length_bytes);
}


// 더티 페이지 처리 함수 (write_fio_log 호출 시 ZNS 제약 검사 수행됨)
void handle_dirty_eviction(int victim_idx) {
    if (victim_idx < 0 || victim_idx >= buffer_size) return;
    if (buffer[victim_idx].page_id != INVALID_PAGE && buffer[victim_idx].is_dirty) {
        // write_fio_log 내부에서 ZNS 순차 쓰기 제약 검사 및 WP 업데이트 수행
        write_fio_log(buffer[victim_idx].page_id * SECTORS_PER_PAGE, SECTORS_PER_PAGE, OP_WRITE);
        buffer[victim_idx].is_dirty = 0; // 쓰기 시도 후 dirty 플래그 해제
    }
}

int evict_fifo() {
    if (buffer_size == 0) return -1;
    int victim_idx = -1;
    unsigned long long min_load_time = ULLONG_MAX;
    for (int i = 0; i < buffer_size; i++) {
        if (buffer[i].page_id != INVALID_PAGE) {
            if (buffer[i].load_time < min_load_time) {
                min_load_time = buffer[i].load_time;
                victim_idx = i;
            }
        }
    }
    if (victim_idx == -1 && find_empty_slot() == -1 && buffer_size > 0) {
        victim_idx = 0;
    }
    return victim_idx;
}

void arc_remove_from_ghost(unsigned long long page_id, unsigned long long* list, int* list_size) {
    int found_idx = -1;
    for (int i = 0; i < *list_size; i++) {
        if (list[i] == page_id) {
            found_idx = i;
            break;
        }
    }
    if (found_idx != -1) {
        for (int i = found_idx; i < *list_size - 1; i++) {
            list[i] = list[i + 1];
        }
        list[*list_size - 1] = INVALID_PAGE;
        (*list_size)--;
    }
}

void arc_add_to_ghost_mru(unsigned long long page_id, unsigned long long* list, int* list_size, int max_ghost_size) {
    if (page_id == INVALID_PAGE || max_ghost_size <= 0) return;

    int existing_idx = -1;
    for (int i = 0; i < *list_size; i++) {
        if (list[i] == page_id) {
            existing_idx = i;
            break;
        }
    }
    if (existing_idx != -1) {
        for (int i = existing_idx; i < *list_size - 1; i++) {
            list[i] = list[i+1];
        }
        (*list_size)--;
    }

    if (*list_size < max_ghost_size) {
        list[*list_size] = page_id;
        (*list_size)++;
    } else if (max_ghost_size > 0) {
        for (int i = 0; i < max_ghost_size - 1; i++) {
            list[i] = list[i + 1];
        }
        list[max_ghost_size - 1] = page_id;
    }
}


int find_in_arc_ghost(unsigned long long page_id, const unsigned long long* list, int list_size) {
    for (int i = 0; i < list_size; i++) {
        if (list[i] == page_id) return 1;
    }
    return 0;
}

int evict_arc_internal_lru(int target_list_type_val) {
    int victim_idx = -1;
    unsigned long long min_access_time = ULLONG_MAX;
    for(int i = 0; i < buffer_size; ++i) {
        if (buffer[i].page_id != INVALID_PAGE && buffer[i].list_type == target_list_type_val) {
             if (buffer[i].last_access_time < min_access_time) {
                 min_access_time = buffer[i].last_access_time;
                 victim_idx = i;
             }
        }
    }
    return victim_idx;
}

int evict_arc_internal_lfu(int target_list_type_val) {
    int victim_idx = -1;
    unsigned int min_access_count = UINT_MAX;
    unsigned long long oldest_load_time = ULLONG_MAX;
    for(int i = 0; i < buffer_size; ++i) {
        if (buffer[i].page_id != INVALID_PAGE && buffer[i].list_type == target_list_type_val) {
            if (buffer[i].access_count < min_access_count) {
                min_access_count = buffer[i].access_count;
                oldest_load_time = buffer[i].load_time;
                victim_idx = i;
            } else if (buffer[i].access_count == min_access_count) {
                if (buffer[i].load_time < oldest_load_time) {
                    oldest_load_time = buffer[i].load_time;
                    victim_idx = i;
                }
            }
        }
    }
    return victim_idx;
}

int evict_via_clock_policy(int *hand_ptr, int list_type_filter_active, int target_list_type, BufferFrame* buffer_frames, int current_buffer_size, const char* policy_name_for_log) {
    if (current_buffer_size == 0) return -1;

    int initial_hand = *hand_ptr;
    int first_pass_victim = -1;

    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < current_buffer_size; ++i) {
            int current_idx = (*hand_ptr + i) % current_buffer_size;

            if (buffer_frames[current_idx].page_id != INVALID_PAGE) {
                if (list_type_filter_active && buffer_frames[current_idx].list_type != target_list_type) {
                    continue;
                }

                if (buffer_frames[current_idx].ref_bit == 0) {
                    *hand_ptr = (current_idx + 1) % current_buffer_size;
                    return current_idx;
                } else {
                    if (pass == 0) {
                        buffer_frames[current_idx].ref_bit = 0;
                        if (first_pass_victim == -1) {
                            first_pass_victim = current_idx;
                        }
                    }
                }
            }
        }
    }

    for (int i = 0; i < current_buffer_size; ++i) {
        int check_idx = (initial_hand + i) % current_buffer_size;
        if (buffer_frames[check_idx].page_id != INVALID_PAGE) {
            if (list_type_filter_active && buffer_frames[check_idx].list_type != target_list_type) {
                continue;
            }
            *hand_ptr = (check_idx + 1) % current_buffer_size;
            fprintf(stderr, "CLOCK Warning (%s): Force evicting page %llu at index %d after full scans (fallback).\n", policy_name_for_log, buffer_frames[check_idx].page_id, check_idx);
            return check_idx;
        }
    }

    fprintf(stderr, "CLOCK Fallback (%s): No valid victim found after all attempts. Using FIFO as last resort.\n", policy_name_for_log);
    return evict_fifo();
}


int arc_find_victim_lru_arc(unsigned long long page_id_to_load) {
    int victim_idx = -1;
    int evict_target_list = 0;
    int t1_plus_t2_size = arc_state.t1_size + arc_state.t2_size;

    if (t1_plus_t2_size == buffer_size) {
        if (find_in_arc_ghost(page_id_to_load, arc_state.b2, arc_state.b2_size) && arc_state.t1_size == arc_state.p) {
            if (arc_state.t2_size > 0) {
                evict_target_list = 2;
            } else if (arc_state.t1_size > 0) {
                evict_target_list = 1;
            } else {
                 return evict_fifo();
            }
        } else {
            if (arc_state.t1_size > 0) {
                evict_target_list = 1;
            } else if (arc_state.t2_size > 0) {
                evict_target_list = 2;
            } else {
                 return evict_fifo();
            }
        }
    } else {
         return evict_fifo();
    }

    victim_idx = evict_arc_internal_lru(evict_target_list);

    if (victim_idx != -1) {
        unsigned long long evicted_page_id = buffer[victim_idx].page_id;
        if (evict_target_list == 1) {
            arc_add_to_ghost_mru(evicted_page_id, arc_state.b1, &arc_state.b1_size, buffer_size);
            if(arc_state.t1_size > 0) arc_state.t1_size--;
        } else {
            arc_add_to_ghost_mru(evicted_page_id, arc_state.b2, &arc_state.b2_size, buffer_size);
            if(arc_state.t2_size > 0) arc_state.t2_size--;
        }
    } else {
        victim_idx = evict_fifo();
        if (victim_idx != -1) {
            if (buffer[victim_idx].list_type == 1 && arc_state.t1_size > 0) arc_state.t1_size--;
            else if (buffer[victim_idx].list_type == 2 && arc_state.t2_size > 0) arc_state.t2_size--;
        }
    }
    return victim_idx;
}

int arc_find_victim_lfu_arc(unsigned long long page_id_to_load) {
    int victim_idx = -1;
    int evict_target_list_type = 0;
    int t3_plus_t4_size = arc_state.t3_size + arc_state.t4_size;

    if (t3_plus_t4_size == buffer_size) {
        if (find_in_arc_ghost(page_id_to_load, arc_state.b4, arc_state.b4_size) && arc_state.t3_size == arc_state.q) {
            if (arc_state.t4_size > 0) {
                evict_target_list_type = 4;
            } else if (arc_state.t3_size > 0) {
                evict_target_list_type = 3;
            } else {
                 return evict_fifo();
            }
        } else {
            if (arc_state.t3_size > 0) {
                evict_target_list_type = 3;
            } else if (arc_state.t4_size > 0) {
                evict_target_list_type = 4;
            } else {
                 return evict_fifo();
            }
        }
    } else {
         return evict_fifo();
    }

    if (evict_target_list_type == 3) {
        victim_idx = evict_arc_internal_lfu(3);
    } else {
        victim_idx = evict_arc_internal_lru(4);
    }

    if (victim_idx != -1) {
        unsigned long long evicted_page_id = buffer[victim_idx].page_id;
        if (evict_target_list_type == 3) {
            arc_add_to_ghost_mru(evicted_page_id, arc_state.b3, &arc_state.b3_size, buffer_size);
            if(arc_state.t3_size > 0) arc_state.t3_size--;
        } else {
            arc_add_to_ghost_mru(evicted_page_id, arc_state.b4, &arc_state.b4_size, buffer_size);
            if(arc_state.t4_size > 0) arc_state.t4_size--;
        }
    } else {
         victim_idx = evict_fifo();
         if (victim_idx != -1) {
            if (buffer[victim_idx].list_type == 3 && arc_state.t3_size > 0) arc_state.t3_size--;
            else if (buffer[victim_idx].list_type == 4 && arc_state.t4_size > 0) arc_state.t4_size--;
         }
    }
    return victim_idx;
}

// 핵심 페이지 접근 함수 (write_fio_log 호출 시 ZNS 제약 검사 수행됨)
void access_page(unsigned long long lba_address, unsigned long long page_id, int operation_type) {
    current_time++;
    int found_idx = find_in_buffer(page_id);
    int target_slot = -1;

    if (page_id == INVALID_PAGE) return;

    // ========================
    //      Cache Hit
    // ========================
    if (found_idx != -1) {
        hits++;
        buffer[found_idx].last_access_time = current_time;
        if (buffer[found_idx].access_count < UINT_MAX) {
            buffer[found_idx].access_count++;
        }
        if (operation_type == OP_WRITE) {
            buffer[found_idx].is_dirty = 1;
        }

        // --- 실제 정책에 따른 히트 처리 --- (enum 심볼 사용으로 자동 대응)
        if (current_policy == LRU_ARC) { // LRU_ARC는 이제 8
            if (buffer[found_idx].list_type == 1) {
                buffer[found_idx].list_type = 2;
                if(arc_state.t1_size > 0) arc_state.t1_size--;
                arc_state.t2_size++;
            }
        } else if (current_policy == LFU_ARC) { // LFU_ARC는 이제 6
            if (buffer[found_idx].list_type == 3) {
                buffer[found_idx].list_type = 4;
                if(arc_state.t3_size > 0) arc_state.t3_size--;
                arc_state.t4_size++;
            }
        }
        // --- 참조용 ARC 상태 업데이트 (LRU/LFU 정책 활성화 시) ---
        else if (current_policy == LRU) { // LRU는 이제 7
            if (buffer[found_idx].ref_arc_list_type == 1) {
                buffer[found_idx].ref_arc_list_type = 2;
                if (arc_state.t1_size > 0) arc_state.t1_size--;
                arc_state.t2_size++;
            }
        } else if (current_policy == LFU) { // LFU는 이제 5
            if (buffer[found_idx].ref_arc_list_type == 3) {
                buffer[found_idx].ref_arc_list_type = 4;
                if (arc_state.t3_size > 0) arc_state.t3_size--;
                arc_state.t4_size++;
            }
        }
        // --- CLOCK 계열 정책 히트 처리 ---
        else if (current_policy == CLOCK_T1 || current_policy == CLOCK_T3 ||
                   current_policy == CLOCK_PRO_T1_B4_LOGS_B2 || current_policy == CLOCK_PRO_T3_B2_LOGS_B4) {
            buffer[found_idx].ref_bit = 1;
        }

    // ========================
    //      Cache Miss
    // ========================
    } else {
        misses++;
        // 읽기 미스 시 디스크 읽기 시뮬레이션 (FIO 로그)
        // 쓰기 미스는 Write Allocate 정책 가정: 먼저 읽고 버퍼에 로드
        write_fio_log(page_id * SECTORS_PER_PAGE, SECTORS_PER_PAGE, OP_READ);

        // --- 미스 발생 시 정책별 처리 ---
        int actual_load_list_type = 0;
        int ref_load_list_type = 0;

        // --- ARC 파라미터 조정 및 로드될 리스트 결정 --- (enum 심볼 사용으로 자동 대응)
        if (current_policy == LRU || current_policy == LRU_ARC) { // LRU는 7, LRU_ARC는 8
            int is_in_b1 = find_in_arc_ghost(page_id, arc_state.b1, arc_state.b1_size);
            int is_in_b2 = find_in_arc_ghost(page_id, arc_state.b2, arc_state.b2_size);

            if (is_in_b1) {
                int delta = (arc_state.b1_size > 0 && arc_state.b2_size >= 0) ? MAX(1, arc_state.b2_size / arc_state.b1_size) : 1;
                delta = MAX(1, delta);
                arc_state.p = MIN(buffer_size, arc_state.p + delta);
                arc_remove_from_ghost(page_id, arc_state.b1, &arc_state.b1_size);
                actual_load_list_type = 2;
                ref_load_list_type = 2;
            } else if (is_in_b2) {
                int delta = (arc_state.b2_size > 0 && arc_state.b1_size >= 0) ? MAX(1, arc_state.b1_size / arc_state.b2_size) : 1;
                delta = MAX(1, delta);
                arc_state.p = MAX(0, arc_state.p - delta);
                arc_remove_from_ghost(page_id, arc_state.b2, &arc_state.b2_size);
                actual_load_list_type = 2;
                ref_load_list_type = 2;
            } else {
                actual_load_list_type = 1;
                ref_load_list_type = 1;
            }
            if (current_policy == LRU) actual_load_list_type = 1; // LRU 캐시는 list_type 1

        } else if (current_policy == LFU || current_policy == LFU_ARC) { // LFU는 5, LFU_ARC는 6
            int is_in_b3 = find_in_arc_ghost(page_id, arc_state.b3, arc_state.b3_size);
            int is_in_b4 = find_in_arc_ghost(page_id, arc_state.b4, arc_state.b4_size);

            if (is_in_b3) {
                int delta = (arc_state.b3_size > 0 && arc_state.b4_size >= 0) ? MAX(1, arc_state.b4_size / arc_state.b3_size) : 1;
                delta = MAX(1, delta);
                arc_state.q = MIN(buffer_size, arc_state.q + delta);
                arc_remove_from_ghost(page_id, arc_state.b3, &arc_state.b3_size);
                actual_load_list_type = 4;
                ref_load_list_type = 4;
            } else if (is_in_b4) {
                int delta = (arc_state.b4_size > 0 && arc_state.b3_size >= 0) ? MAX(1, arc_state.b3_size / arc_state.b4_size) : 1;
                delta = MAX(1, delta);
                arc_state.q = MAX(0, arc_state.q - delta);
                arc_remove_from_ghost(page_id, arc_state.b4, &arc_state.b4_size);
                actual_load_list_type = 4;
                ref_load_list_type = 4;
            } else {
                actual_load_list_type = 3;
                ref_load_list_type = 3;
            }
            if (current_policy == LFU) actual_load_list_type = 3; // LFU 캐시는 list_type 3

        } else if (current_policy == CLOCK_T1) { // CLOCK_T1은 2
            actual_load_list_type = 1;
        } else if (current_policy == CLOCK_T3) { // CLOCK_T3은 3
            actual_load_list_type = 3;
        } else if (current_policy == CLOCK_PRO_T1_B4_LOGS_B2) { // CLOCK_PRO_T1...은 0
            actual_load_list_type = 1;
            if (find_in_arc_ghost(page_id, arc_state.b4, arc_state.b4_size)) {
                int delta_val = (arc_state.t1_size > 0 && arc_state.b4_size > 0) ? MAX(1, arc_state.t1_size / arc_state.b4_size) : 1;
                if (arc_state.t1_size == 0 && arc_state.b4_size > 0 && buffer_size > 0) delta_val = MAX(1, buffer_size / arc_state.b4_size);
                delta_val = MAX(1, delta_val);
                arc_state.p = MIN(buffer_size, arc_state.p + delta_val);
                arc_remove_from_ghost(page_id, arc_state.b4, &arc_state.b4_size);
            } else {
                int delta_val = (arc_state.t1_size > 0 && arc_state.b4_size > 0) ? MAX(1, arc_state.b4_size / arc_state.t1_size) : 1;
                if (arc_state.b4_size == 0 && arc_state.t1_size > 0 && buffer_size > 0) delta_val = MAX(1, buffer_size / arc_state.t1_size);
                delta_val = MAX(1, delta_val);
                arc_state.p = MAX(0, arc_state.p - delta_val);
            }
        } else if (current_policy == CLOCK_PRO_T3_B2_LOGS_B4) { // CLOCK_PRO_T3...은 1
            actual_load_list_type = 3;
            if (find_in_arc_ghost(page_id, arc_state.b2, arc_state.b2_size)) {
                int delta_val = (arc_state.t3_size > 0 && arc_state.b2_size > 0) ? MAX(1, arc_state.t3_size / arc_state.b2_size) : 1;
                if (arc_state.t3_size == 0 && arc_state.b2_size > 0 && buffer_size > 0) delta_val = MAX(1, buffer_size / arc_state.b2_size);
                delta_val = MAX(1, delta_val);
                arc_state.q = MIN(buffer_size, arc_state.q + delta_val);
                arc_remove_from_ghost(page_id, arc_state.b2, &arc_state.b2_size);
            } else {
                int delta_val = (arc_state.t3_size > 0 && arc_state.b2_size > 0) ? MAX(1, arc_state.b2_size / arc_state.t3_size) : 1;
                if (arc_state.b2_size == 0 && arc_state.t3_size > 0 && buffer_size > 0) delta_val = MAX(1, buffer_size / arc_state.t3_size);
                delta_val = MAX(1, delta_val);
                arc_state.q = MAX(0, arc_state.q - delta_val);
            }
        }
        // FIFO (이제 4)는 별도의 actual_load_list_type 설정 로직이 이 블록에 없음.
        // 아래 새 페이지 로드 시 FIFO의 list_type은 0으로 설정됨.

        // --- 버퍼 공간 확보 (Eviction) --- (enum 심볼 사용으로 자동 대응)
        target_slot = find_empty_slot();
        if (target_slot == -1) {
            int victim_idx = -1;
            unsigned long long evicted_page_id = INVALID_PAGE;

            // 정책별 희생자 선택
            if (current_policy == FIFO) victim_idx = evict_fifo(); // FIFO는 4
            else if (current_policy == LRU) victim_idx = evict_arc_internal_lru(1); // LRU는 7
            else if (current_policy == LFU) victim_idx = evict_arc_internal_lfu(3); // LFU는 5
            else if (current_policy == LRU_ARC) victim_idx = arc_find_victim_lru_arc(page_id); // LRU_ARC는 8
            else if (current_policy == LFU_ARC) victim_idx = arc_find_victim_lfu_arc(page_id); // LFU_ARC는 6
            else if (current_policy == CLOCK_T1) victim_idx = evict_via_clock_policy(&global_clk_hand, 0, 0, buffer, buffer_size, policy_names[current_policy]); // CLOCK_T1은 2
            else if (current_policy == CLOCK_T3) victim_idx = evict_via_clock_policy(&global_clk_hand, 0, 0, buffer, buffer_size, policy_names[current_policy]); // CLOCK_T3은 3
            else if (current_policy == CLOCK_PRO_T1_B4_LOGS_B2) { // CLOCK_PRO_T1...은 0
                 while (arc_state.t1_size >= arc_state.p && arc_state.t1_size > 0) {
                     victim_idx = evict_via_clock_policy(&arc_state.p_clk_hand, 1, 1, buffer, buffer_size, policy_names[current_policy]);
                     if (victim_idx != -1) break;
                     fprintf(stderr, "CLOCK_PRO_T1 Warning: Could not find victim in T1 despite T1 size >= p. Check state.\n");
                     break;
                 }
                 if (victim_idx == -1) {
                     victim_idx = evict_via_clock_policy(&arc_state.p_clk_hand, 0, 0, buffer, buffer_size, "CLOCK_PRO_T1_Fallback");
                 }
            } else if (current_policy == CLOCK_PRO_T3_B2_LOGS_B4) { // CLOCK_PRO_T3...은 1
                 while (arc_state.t3_size >= arc_state.q && arc_state.t3_size > 0) {
                     victim_idx = evict_via_clock_policy(&arc_state.q_clk_hand, 1, 3, buffer, buffer_size, policy_names[current_policy]);
                     if (victim_idx != -1) break;
                     fprintf(stderr, "CLOCK_PRO_T3 Warning: Could not find victim in T3 despite T3 size >= q. Check state.\n");
                     break;
                 }
                 if (victim_idx == -1) {
                     victim_idx = evict_via_clock_policy(&arc_state.q_clk_hand, 0, 0, buffer, buffer_size, "CLOCK_PRO_T3_Fallback");
                 }
            }

            // 최종 희생자 선택 실패 시 Fallback
            if (victim_idx == -1 && buffer_size > 0) {
                 victim_idx = evict_fifo();
                 if (victim_idx != -1 && buffer[victim_idx].page_id != INVALID_PAGE) {
                     int list_of_fifo_victim = buffer[victim_idx].list_type;
                     if (current_policy == LRU_ARC || current_policy == LRU || current_policy == CLOCK_PRO_T1_B4_LOGS_B2 || current_policy == CLOCK_T1) {
                         if (list_of_fifo_victim == 1 && arc_state.t1_size > 0) arc_state.t1_size--;
                         else if (list_of_fifo_victim == 2 && arc_state.t2_size > 0) arc_state.t2_size--;
                     }
                     if (current_policy == LFU_ARC || current_policy == LFU || current_policy == CLOCK_PRO_T3_B2_LOGS_B4 || current_policy == CLOCK_T3) {
                         if (list_of_fifo_victim == 3 && arc_state.t3_size > 0) arc_state.t3_size--;
                         else if (list_of_fifo_victim == 4 && arc_state.t4_size > 0) arc_state.t4_size--;
                     }
                 }
            }

            // 희생자 처리
            if (victim_idx != -1) {
                handle_dirty_eviction(victim_idx); // ZNS 제약 검사는 handle_dirty_eviction -> write_fio_log 에서 처리
                evicted_page_id = buffer[victim_idx].page_id;

                // --- 정책별 고스트 리스트 및 로그/히스토리 업데이트 --- (enum 심볼 사용으로 자동 대응)
                if (current_policy == LRU && evicted_page_id != INVALID_PAGE) { // LRU는 7
                    if (buffer[victim_idx].ref_arc_list_type == 1) {
                        arc_add_to_ghost_mru(evicted_page_id, arc_state.b1, &arc_state.b1_size, buffer_size);
                        if(arc_state.t1_size > 0) arc_state.t1_size--;
                    } else if (buffer[victim_idx].ref_arc_list_type == 2) {
                        arc_add_to_ghost_mru(evicted_page_id, arc_state.b2, &arc_state.b2_size, buffer_size);
                        if(arc_state.t2_size > 0) arc_state.t2_size--;
                    }
                } else if (current_policy == LFU && evicted_page_id != INVALID_PAGE) { // LFU는 5
                     if (buffer[victim_idx].ref_arc_list_type == 3) {
                        arc_add_to_ghost_mru(evicted_page_id, arc_state.b3, &arc_state.b3_size, buffer_size);
                        if(arc_state.t3_size > 0) arc_state.t3_size--;
                    } else if (buffer[victim_idx].ref_arc_list_type == 4) {
                        arc_add_to_ghost_mru(evicted_page_id, arc_state.b4, &arc_state.b4_size, buffer_size);
                        if(arc_state.t4_size > 0) arc_state.t4_size--;
                    }
                }
                else if (current_policy == CLOCK_PRO_T1_B4_LOGS_B2 && evicted_page_id != INVALID_PAGE) { // CLOCK_PRO_T1...은 0
                    arc_add_to_ghost_mru(evicted_page_id, arc_state.b4, &arc_state.b4_size, buffer_size); // B4는 히스토리
                    arc_add_to_ghost_mru(evicted_page_id, arc_state.b2, &arc_state.b2_size, buffer_size); // B2는 로그
                    if(arc_state.t1_size > 0 && buffer[victim_idx].list_type == 1) arc_state.t1_size--;
                } else if (current_policy == CLOCK_PRO_T3_B2_LOGS_B4 && evicted_page_id != INVALID_PAGE) { // CLOCK_PRO_T3...은 1
                    arc_add_to_ghost_mru(evicted_page_id, arc_state.b2, &arc_state.b2_size, buffer_size); // B2는 히스토리
                    arc_add_to_ghost_mru(evicted_page_id, arc_state.b4, &arc_state.b4_size, buffer_size); // B4는 로그
                    if(arc_state.t3_size > 0 && buffer[victim_idx].list_type == 3) arc_state.t3_size--;
                }

                buffer[victim_idx].page_id = INVALID_PAGE;
                target_slot = victim_idx;
            }
        }

        // --- 새 페이지 로드 ---
        if (target_slot != -1) {
            buffer[target_slot].page_id = page_id;
            buffer[target_slot].load_time = current_time;
            buffer[target_slot].last_access_time = current_time;
            buffer[target_slot].access_count = 1;
            buffer[target_slot].is_dirty = (operation_type == OP_WRITE); // 쓰기 미스 시 dirty 설정 (Write Allocate)
            buffer[target_slot].list_type = actual_load_list_type; // 위에서 결정된 actual_load_list_type 사용
            buffer[target_slot].ref_bit = 1; // CLOCK 계열을 위해 기본적으로 1로 설정

            // 정책별 리스트 크기 및 참조 상태 업데이트 (enum 심볼 사용으로 자동 대응)
            if (current_policy == LRU) { // LRU는 7
                buffer[target_slot].ref_arc_list_type = ref_load_list_type;
                if (ref_load_list_type == 1) arc_state.t1_size++; else if (ref_load_list_type == 2) arc_state.t2_size++;
            } else if (current_policy == LFU) { // LFU는 5
                buffer[target_slot].ref_arc_list_type = ref_load_list_type;
                if (ref_load_list_type == 3) arc_state.t3_size++; else if (ref_load_list_type == 4) arc_state.t4_size++;
            }
            else if (current_policy == LRU_ARC) { // LRU_ARC는 8
                buffer[target_slot].ref_arc_list_type = 0; // ARC 자체이므로 ref_arc_list_type 불필요
                if (actual_load_list_type == 1) arc_state.t1_size++; else if (actual_load_list_type == 2) arc_state.t2_size++;
            } else if (current_policy == LFU_ARC) { // LFU_ARC는 6
                buffer[target_slot].ref_arc_list_type = 0; // ARC 자체이므로 ref_arc_list_type 불필요
                if (actual_load_list_type == 3) arc_state.t3_size++; else if (actual_load_list_type == 4) arc_state.t4_size++;
            } else if (current_policy == CLOCK_T1) { // CLOCK_T1은 2
                 buffer[target_slot].ref_arc_list_type = 0;
                 buffer[target_slot].list_type = 1; // T1 캐시
                 if (arc_state.t1_size < buffer_size) arc_state.t1_size++;
            } else if (current_policy == CLOCK_T3) { // CLOCK_T3은 3
                 buffer[target_slot].ref_arc_list_type = 0;
                 buffer[target_slot].list_type = 3; // T3 캐시
                 if (arc_state.t3_size < buffer_size) arc_state.t3_size++;
            } else if (current_policy == CLOCK_PRO_T1_B4_LOGS_B2) { // CLOCK_PRO_T1...은 0
                buffer[target_slot].ref_arc_list_type = 0;
                buffer[target_slot].list_type = 1; // T1 캐시
                if (arc_state.t1_size < buffer_size) arc_state.t1_size++;
                arc_add_to_ghost_mru(page_id, arc_state.b2, &arc_state.b2_size, buffer_size); // b2는 로그
            } else if (current_policy == CLOCK_PRO_T3_B2_LOGS_B4) { // CLOCK_PRO_T3...은 1
                buffer[target_slot].ref_arc_list_type = 0;
                buffer[target_slot].list_type = 3; // T3 캐시
                if (arc_state.t3_size < buffer_size) arc_state.t3_size++;
                arc_add_to_ghost_mru(page_id, arc_state.b4, &arc_state.b4_size, buffer_size); // b4는 로그
            } else if (current_policy == FIFO) { // FIFO는 4
                 buffer[target_slot].ref_arc_list_type = 0;
                 buffer[target_slot].ref_bit = 0; // FIFO는 ref_bit 사용 안 함
                 buffer[target_slot].list_type = 0; // FIFO의 경우 list_type을 0으로 설정 (실제 정책과 무관한 기본값)
                                                     // 또는 FIFO 고유 list_type (예: 4)을 사용하려면 여기서 설정
            }
        } else if (buffer_size > 0) {
            // fprintf(stderr, "CRITICAL Error: Failed to find or create a slot for page %llu. Policy: %s\n", page_id, policy_names[current_policy]);
        }
    } // End Cache Miss
}

/* // 버퍼 상태 출력 함수 전체 주석 처리 시작
void print_buffer_state() {
    // ... (내용 동일) ...
}
*/ // 버퍼 상태 출력 함수 전체 주석 처리 끝


int main(int argc, char *argv[]) {
    // 인수 개수 확인
    if (argc < 5) {
        fprintf(stderr, "사용법: %s <버퍼_크기> <초기_정책_이름> <워크로드_파일명> <존_크기_페이지>\n", argv[0]);
        // 사용 가능 정책 목록 업데이트
        fprintf(stderr, "사용 가능 정책 (이름): CLOCK_PRO_T1_B4_LOGS_B2, CLOCK_PRO_T3_B2_LOGS_B4, CLOCK_T1, CLOCK_T3, FIFO, LFU, LFU_ARC, LRU, LRU_ARC\n");
        fprintf(stderr, "워크로드 파일 내 정책 변경: P <정책코드> (0..8)\n"); // 정책 코드 범위 업데이트
        fprintf(stderr, "존_크기_페이지: 존 하나당 페이지 수 (0이면 ZNS 비활성화)\n");
        return 1;
    }

    if (SECTORS_PER_PAGE <= 0 || SECTOR_SIZE <= 0 || MAX_BUFFER_SIZE <= 0) {
         fprintf(stderr, "설정 오류: SECTORS_PER_PAGE, SECTOR_SIZE, MAX_BUFFER_SIZE는 양수여야 합니다.\n"); return 1;
    }

    // 버퍼 크기 파싱
    char *endptr;
    long val_bs = strtol(argv[1], &endptr, 10);
    if (endptr == argv[1] || *endptr != '\0' || val_bs <= 0 || val_bs > MAX_BUFFER_SIZE) {
         fprintf(stderr, "오류: 잘못된 버퍼 크기 '%s'. 1과 %d 사이여야 합니다.\n", argv[1], MAX_BUFFER_SIZE); return 1;
    }
    buffer_size = (int)val_bs;

    // 초기 정책 이름 파싱
    char* initial_policy_arg_orig = argv[2];
    char initial_policy_arg_lower[256];
    strncpy(initial_policy_arg_lower, initial_policy_arg_orig, sizeof(initial_policy_arg_lower) -1);
    initial_policy_arg_lower[sizeof(initial_policy_arg_lower)-1] = '\0';
    for (char *p = initial_policy_arg_lower; *p; ++p) *p = tolower(*p);

    // 정책 이름 비교 및 설정 (새로운 순서와 값에 맞게)
    // enum 심볼을 사용하므로, strcmp만 정확하면 current_policy에 올바른 enum 값이 할당됨.
    if (strcmp(initial_policy_arg_lower, "clock_pro_t1_b4_logs_b2") == 0) current_policy = CLOCK_PRO_T1_B4_LOGS_B2; // 0
    else if (strcmp(initial_policy_arg_lower, "clock_pro_t3_b2_logs_b4") == 0) current_policy = CLOCK_PRO_T3_B2_LOGS_B4; // 1
    else if (strcmp(initial_policy_arg_lower, "clock_t1") == 0) current_policy = CLOCK_T1; // 2
    else if (strcmp(initial_policy_arg_lower, "clock_t3") == 0) current_policy = CLOCK_T3; // 3
    else if (strcmp(initial_policy_arg_lower, "fifo") == 0) current_policy = FIFO;       // 4
    else if (strcmp(initial_policy_arg_lower, "lfu") == 0) current_policy = LFU;         // 5
    else if (strcmp(initial_policy_arg_lower, "lfu_arc") == 0) current_policy = LFU_ARC; // 6
    else if (strcmp(initial_policy_arg_lower, "lru") == 0) current_policy = LRU;         // 7
    else if (strcmp(initial_policy_arg_lower, "lru_arc") == 0) current_policy = LRU_ARC; // 8
    else {
        fprintf(stderr, "경고: 잘못된 초기 정책 이름 '%s'. 기본 정책인 FIFO로 설정합니다.\n", initial_policy_arg_orig);
        current_policy = FIFO; // 기본값 FIFO (새로운 값 4)
    }
    previous_policy_for_state_carryover = current_policy;


    // 워크로드 파일명
    char* filename = argv[3];

    // 존 크기 파싱
    errno = 0;
    unsigned long long val_zs = strtoull(argv[4], &endptr, 10);
    if (endptr == argv[4] || *endptr != '\0' || errno != 0 || val_zs == ULLONG_MAX) { // 0도 허용 (ZNS 비활성화)
        fprintf(stderr, "오류: 잘못된 존 크기 '%s'. 0 또는 양수여야 합니다.\n", argv[4]);
        if (errno != 0) perror("strtoull 오류");
        return 1;
    }
    zone_size_pages_global = val_zs;

    // 워크로드 파일 열기
    FILE *infile = fopen(filename, "r");
    if (infile == NULL) { fprintf(stderr, "오류: 워크로드 파일 '%s' 열기 실패: %s\n", filename, strerror(errno)); return 1;}

    // 로그 파일 이름 설정
    char log_filename[MAX_FILENAME_LEN];
    // policy_names[current_policy]는 새로운 배열 순서에 따라 올바른 이름을 가져옴.
    if (zone_size_pages_global > 0) {
        snprintf(log_filename, sizeof(log_filename), "%s_%s_%d_ZS%llu.fio.log",
                 filename, policy_names[current_policy], buffer_size, zone_size_pages_global);
    } else {
        snprintf(log_filename, sizeof(log_filename), "%s_%s_%d.fio.log",
                 filename, policy_names[current_policy], buffer_size);
    }
    log_file = fopen(log_filename, "w");
    if (log_file == NULL) { fprintf(stderr, "오류: 로그 파일 '%s' 열기 실패: %s\n", log_filename, strerror(errno)); fclose(infile); return 1;}
    printf("FIO 트레이스를 다음 파일에 로깅합니다: %s\n", log_filename);
    fprintf(log_file, "fio version 2 iolog\n%s add\n%s open\n", DEVICE_NAME, DEVICE_NAME);

    // 시뮬레이션 정보 출력
    printf("--- 시뮬레이션 시작 (초기 정책: %s) ---\n", policy_names[current_policy]);
    if (zone_size_pages_global > 0) {
        printf("정책: %s, 버퍼 크기: %d 프레임, 워크로드 파일: %s, 존 크기: %llu 페이지 (ZNS 활성)\n",
               policy_names[current_policy], buffer_size, filename, zone_size_pages_global);
    } else {
         printf("정책: %s, 버퍼 크기: %d 프레임, 워크로드 파일: %s (ZNS 비활성)\n",
               policy_names[current_policy], buffer_size, filename);
    }
    printf("페이지 설정: 페이지당 %d 섹터, 섹터당 %d 바이트 (%d KB/페이지)\n",
           SECTORS_PER_PAGE, SECTOR_SIZE, (SECTORS_PER_PAGE * SECTOR_SIZE) / 1024);

    // 초기화
    initialize_buffer(); // previous_policy_for_state_carryover는 main에서 current_policy 설정 후 다시 설정됨.
    initialize_arc_state(1); // current_policy에 따라 ARC 상태 초기화
    initialize_zone_write_pointers(); // Zone 쓰기 포인터 초기화

    // 워크로드 처리 루프
    char line_buffer[256];
    int line_num = 0;
    unsigned long long total_lba_requests_processed = 0;

    printf("요청 처리 중 (형식: LBA Op 또는 P policy_code)...\n");
    while (fgets(line_buffer, sizeof(line_buffer), infile) != NULL) {
        line_num++;
        char *trimmed_line = line_buffer;
        while (isspace((unsigned char)*trimmed_line)) trimmed_line++;
        char *end_of_line = trimmed_line + strlen(trimmed_line) - 1;
        while (end_of_line > trimmed_line && isspace((unsigned char)*end_of_line)) end_of_line--;
        *(end_of_line + 1) = '\0';

        if (trimmed_line[0] == '\0' || trimmed_line[0] == '#') continue;

        // 정책 변경 명령어 처리
        if (toupper(trimmed_line[0]) == 'P' && (trimmed_line[1] == ' ' || trimmed_line[1] == '\t')) {
            int new_policy_code; char cmd_char;
            if (sscanf(trimmed_line, "%c %d", &cmd_char, &new_policy_code) == 2 && toupper(cmd_char) == 'P') {
                // 수정된 정책 코드 유효 범위 확인: 0부터 8까지
                if (new_policy_code >= CLOCK_PRO_T1_B4_LOGS_B2 && new_policy_code <= LRU_ARC) {
                    ReplacementPolicy old_policy = current_policy;
                    ReplacementPolicy new_policy = (ReplacementPolicy)new_policy_code;
                    if (old_policy != new_policy) {
                        printf("\nINFO: (라인 %d) 정책 변경 감지: %s ===> %s\n",
                               line_num, policy_names[old_policy], policy_names[new_policy]);
                        previous_policy_for_state_carryover = old_policy;
                        current_policy = new_policy; // current_policy 업데이트

                        int reset_arc_completely = 1;
                        // 상태 이전 로직 (enum 심볼 사용으로 자동 대응)
                        if ((old_policy == LRU && new_policy == LRU_ARC) || (old_policy == LRU_ARC && new_policy == LRU)) {
                            reset_arc_completely = 0;
                            printf("INFO: LRU <-> LRU_ARC 전환. p, B1, B2 상태를 이어받습니다.\n");
                        }
                        else if ((old_policy == LFU && new_policy == LFU_ARC) || (old_policy == LFU_ARC && new_policy == LFU)) {
                            reset_arc_completely = 0;
                            printf("INFO: LFU <-> LFU_ARC 전환. q, B3, B4 상태를 이어받습니다.\n");
                        }

                        initialize_arc_state(reset_arc_completely); // 변경된 current_policy에 따라 ARC 상태 다시 초기화/조정

                        // 버퍼 프레임 상태 재설정 (list_type, ref_arc_list_type, t1/t2/t3/t4_size 조정)
                        // 이 부분은 current_policy (new_policy)를 기준으로 동작하므로 enum 심볼에 의해 자동 대응.
                        // 다만, 각 정책에 할당되는 list_type 값의 의도를 다시 한번 확인하는 것이 좋음.
                        arc_state.t1_size = 0; arc_state.t2_size = 0;
                        arc_state.t3_size = 0; arc_state.t4_size = 0;

                        for (int i = 0; i < buffer_size; i++) {
                            if (buffer[i].page_id != INVALID_PAGE) {
                                buffer[i].ref_bit = 0; // 기본적으로 ref_bit 초기화
                                buffer[i].ref_arc_list_type = 0; // 기본적으로 초기화

                                if (new_policy == LRU) { // LRU는 7
                                    buffer[i].list_type = 1; // LRU의 주 캐시 리스트 타입
                                    if (old_policy == LRU_ARC && (buffer[i].list_type == 1 || buffer[i].list_type == 2)) { // 이전 list_type을 ref_arc_list_type으로 사용
                                         buffer[i].ref_arc_list_type = buffer[i].list_type; // 이전 list_type 값이 1 또는 2였을 것임.
                                    } else {
                                        buffer[i].ref_arc_list_type = 1; // 기본 T1_ref
                                    }
                                    if(buffer[i].ref_arc_list_type == 1) arc_state.t1_size++; else arc_state.t2_size++;
                                } else if (new_policy == LFU) { // LFU는 5
                                    buffer[i].list_type = 3; // LFU의 주 캐시 리스트 타입
                                    if (old_policy == LFU_ARC && (buffer[i].list_type == 3 || buffer[i].list_type == 4)) {
                                         buffer[i].ref_arc_list_type = buffer[i].list_type;
                                    } else {
                                        buffer[i].ref_arc_list_type = 3; // 기본 T3_ref
                                    }
                                    if(buffer[i].ref_arc_list_type == 3) arc_state.t3_size++; else arc_state.t4_size++;
                                } else if (new_policy == LRU_ARC) { // LRU_ARC는 8
                                    if (old_policy == LRU && (buffer[i].ref_arc_list_type == 1 || buffer[i].ref_arc_list_type == 2)) {
                                        buffer[i].list_type = buffer[i].ref_arc_list_type; // LRU의 ref_arc_list_type을 list_type으로
                                    } else {
                                        buffer[i].list_type = 1; // 기본 T1
                                    }
                                    if(buffer[i].list_type == 1) arc_state.t1_size++; else arc_state.t2_size++;
                                } else if (new_policy == LFU_ARC) { // LFU_ARC는 6
                                     if (old_policy == LFU && (buffer[i].ref_arc_list_type == 3 || buffer[i].ref_arc_list_type == 4)) {
                                        buffer[i].list_type = buffer[i].ref_arc_list_type;
                                    } else {
                                        buffer[i].list_type = 3; // 기본 T3
                                    }
                                    if(buffer[i].list_type == 3) arc_state.t3_size++; else arc_state.t4_size++;
                                } else if (new_policy == CLOCK_T1 || new_policy == CLOCK_PRO_T1_B4_LOGS_B2) { // CLOCK_T1=2, CLOCK_PRO_T1=0
                                    buffer[i].list_type = 1; // T1 캐시
                                    arc_state.t1_size++;
                                    buffer[i].ref_bit = 1; // CLOCK 계열은 ref_bit 1로 시작
                                } else if (new_policy == CLOCK_T3 || new_policy == CLOCK_PRO_T3_B2_LOGS_B4) { // CLOCK_T3=3, CLOCK_PRO_T3=1
                                    buffer[i].list_type = 3; // T3 캐시
                                    arc_state.t3_size++;
                                    buffer[i].ref_bit = 1; // CLOCK 계열은 ref_bit 1로 시작
                                } else if (new_policy == FIFO) { // FIFO는 4
                                    buffer[i].list_type = 0; // FIFO는 별도 list_type 구분 없음 (또는 FIFO 고유값 사용)
                                    // FIFO는 ref_bit 사용 안하므로 위에서 0으로 초기화된 값 유지.
                                }
                            }
                        }
                        arc_state.p_clk_hand = 0; arc_state.q_clk_hand = 0; global_clk_hand = 0;

                        printf("--- 정책 변경 완료: %s ---\n", policy_names[current_policy]);
                        // print_buffer_state();
                    }
                } else { fprintf(stderr, "경고: (라인 %d) 잘못된 정책 코드 %d. 유효 범위: %d-%d. 무시.\n", line_num, new_policy_code, CLOCK_PRO_T1_B4_LOGS_B2, LRU_ARC); }
            } else { fprintf(stderr, "경고: (라인 %d) 잘못된 정책 변경 명령어 형식. 무시. 내용: [%s]\n", line_num, trimmed_line); }
        }
        // LBA 접근 요청 처리
        else {
            unsigned long long lba_address_val; char op_char_val_arr[3]; int operation_type_val;
            if (sscanf(trimmed_line, "%llu %2s", &lba_address_val, op_char_val_arr) == 2) {
                char op_char_val = tolower(op_char_val_arr[0]);
                if (op_char_val == 'r') operation_type_val = OP_READ;
                else if (op_char_val == 'w') operation_type_val = OP_WRITE;
                else { fprintf(stderr, "경고: (라인 %d) 잘못된 작업 유형 '%c'. 건너<0xEB><0x9C><0x84>니다.\n", line_num, op_char_val_arr[0]); continue; }

                unsigned long long page_id_val = lba_to_page_id(lba_address_val);
                access_page(lba_address_val, page_id_val, operation_type_val); // ZNS 검사는 access_page -> handle_dirty_eviction -> write_fio_log 에서 처리됨
                total_lba_requests_processed++;

                if (total_lba_requests_processed > 0 && total_lba_requests_processed % 1000000 == 0) {
                     printf("  %llu개 LBA 요청 처리 완료 (현재 정책: %s)...\n", total_lba_requests_processed, policy_names[current_policy]);
                }
            } else { fprintf(stderr, "경고: (라인 %d) 잘못된 LBA 접근 요청 형식. 무시. 내용: [%s]\n", line_num, trimmed_line); }
        }
    } // End while loop

    // 파일 읽기 오류 확인
    if (ferror(infile)) { fprintf(stderr, "\n워크로드 파일 '%s' 읽기 오류 발생: %s\n", filename, strerror(errno)); }
    printf("총 %llu개의 LBA 요청 처리 완료.\n", total_lba_requests_processed);

    // 시뮬레이션 종료 전 더티 페이지 플러시
    printf("시뮬레이션 종료 시 남은 더티 페이지 플러시 중...\n");
    int dirty_flushed = 0;
    for (int i = 0; i < buffer_size; ++i) {
        if (buffer[i].page_id != INVALID_PAGE && buffer[i].is_dirty) {
             handle_dirty_eviction(i); // 내부적으로 ZNS 제약 검사 수행
             dirty_flushed++;
        }
    }
    if (dirty_flushed > 0) printf("%d개의 더티 페이지를 플러시했습니다.\n", dirty_flushed);
    else printf("플러시할 더티 페이지가 버퍼에 남아있지 않습니다.\n");

    // 파일 닫기
    if (log_file != NULL) { fprintf(log_file, "%s close\n", DEVICE_NAME); fclose(log_file); log_file = NULL; }
    if (infile != NULL) fclose(infile);

    // 최종 상태 출력
    printf("--- 최종 상태 --- \n");
    // print_buffer_state();
    printf("--- 시뮬레이션 종료 ---\n");

    // 결과 요약 계산
    long long total_accesses = hits + misses;
    double hit_rate = (total_accesses == 0) ? 0.0 : (double)hits / total_accesses * 100.0;

    // 요약 출력 시 사용할 초기 정책 이름 결정
    const char* summary_initial_policy_name_to_print = initial_policy_arg_orig;
    int initial_policy_was_valid = 0;
    // policy_names 배열과 그 크기가 변경되었으므로, num_policies도 이를 반영해야 함.
    int num_policies = sizeof(policy_names) / sizeof(policy_names[0]); // 현재 9개 정책
     for (int i = 0; i < num_policies; ++i) {
        char temp_policy_name_lower[256];
        strncpy(temp_policy_name_lower, policy_names[i], sizeof(temp_policy_name_lower) -1);
        temp_policy_name_lower[sizeof(temp_policy_name_lower)-1] = '\0';
        for(char *p_temp = temp_policy_name_lower; *p_temp; ++p_temp) *p_temp = tolower(*p_temp);
        if (strcmp(initial_policy_arg_lower, temp_policy_name_lower) == 0) {
            initial_policy_was_valid = 1;
            break;
        }
    }
    if (!initial_policy_was_valid) {
        summary_initial_policy_name_to_print = policy_names[FIFO]; // FIFO는 이제 policy_names[4]
    }

    // 최종 결과 요약 출력
    printf("====================================================================================\n");
    printf("                         시뮬레이션 결과 요약\n");
    printf("------------------------------------------------------------------------------------\n");
    printf(" 초기 정책:       %-20s | 버퍼 크기:    %-5d 프레임\n", summary_initial_policy_name_to_print, buffer_size);
    if (zone_size_pages_global > 0) {
        printf(" 워크로드 파일:   %-30s | 존 크기:      %llu 페이지 (ZNS 활성)\n", filename, zone_size_pages_global);
    } else {
        printf(" 워크로드 파일:   %-30s | (ZNS 비활성)\n", filename);
    }
    printf(" 총 LBA 요청 수:  %-12llu | 캐시 히트 수:   %-12lld\n", total_lba_requests_processed, hits);
    printf(" 캐시 미스 수:   %-12lld | 총 접근 수:     %-12lld (히트+미스)\n", misses, total_accesses);
    printf(" 히트율:        %6.2f%%\n", hit_rate);
    printf("------------------------------------------------------------------------------------\n");
    printf(" (참고: 미스 카운트에는 쓰기 미스 시 초기 필수 읽기(쓰기 할당)가 포함됩니다.)\n");
    printf(" (참고: ZNS 활성 시 비순차 쓰기는 stderr로 경고/오류 출력 후 로그에는 기록될 수 있습니다.)\n");
    printf(" (FIO 로그 생성됨: %s)\n", log_filename);
    printf("====================================================================================\n");

    return 0;
}
