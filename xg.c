#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <xgboost/c_api.h>

// ===================================================================
// 0. Error Handling Macro for XGBoost Calls
// ===================================================================
#define safe_xgboost(call) { \
    int err = (call); \
    if (err != 0) { \
        fprintf(stderr, "%s:%d: error in %s: %s\n", __FILE__, __LINE__, #call, XGBGetLastError()); \
        exit(1); \
    } \
}

// ===================================================================
// 1. 특징(Features) 및 데이터 저장을 위한 구조체 정의
// ===================================================================
typedef struct {
    double read_ratio;
    double avg_reuse_distance;
    double max_reuse_distance;
    double access_locality;
    double unique_address_ratio;
    double entropy;
    double rw_switch_rate;
    double seq_access_ratio;
} TraceFeatures;

// ===================================================================
// 2. 해시 테이블 구현
// ===================================================================
typedef struct HashNode {
    long key;
    int last_index;
    int count;
    struct HashNode* next;
} HashNode;

typedef struct HashTable {
    int size;
    int item_count;
    HashNode** buckets;
} HashTable;

// 해시 함수
unsigned int hash_function(long key, int size) {
    return (unsigned int)(key % size);
}

// 새 해시 테이블 생성
HashTable* create_hash_table(int size) {
    HashTable* table = (HashTable*)malloc(sizeof(HashTable));
    if (!table) {
        perror("Failed to allocate HashTable");
        exit(EXIT_FAILURE);
    }
    table->size = size;
    table->item_count = 0;
    table->buckets = (HashNode**)calloc(size, sizeof(HashNode*));
    if (!table->buckets) {
        perror("Failed to allocate HashTable buckets");
        free(table);
        exit(EXIT_FAILURE);
    }
    return table;
}

// 해시 테이블에 노드 삽입 또는 업데이트
HashNode* insert_hash_node(HashTable* table, long key, int current_index) {
    unsigned int bucket_index = hash_function(key, table->size);
    HashNode* current = table->buckets[bucket_index];
    HashNode* prev = NULL;

    while (current != NULL) {
        if (current->key == key) {
            current->count++;
            return current;
        }
        prev = current;
        current = current->next;
    }

    HashNode* newNode = (HashNode*)malloc(sizeof(HashNode));
    if (!newNode) {
        perror("Failed to allocate HashNode");
        return NULL; 
    }
    newNode->key = key;
    newNode->last_index = current_index;
    newNode->count = 1;
    newNode->next = NULL;

    if (prev == NULL) {
        table->buckets[bucket_index] = newNode;
    } else {
        prev->next = newNode;
    }
    table->item_count++;
    return newNode;
}

// 해시 테이블에서 노드 검색
HashNode* find_hash_node(HashTable* table, long key) {
    unsigned int bucket_index = hash_function(key, table->size);
    HashNode* current = table->buckets[bucket_index];
    while (current != NULL) {
        if (current->key == key) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// 해시 테이블 메모리 해제
void free_hash_table(HashTable* table) {
    if (!table) return;
    for (int i = 0; i < table->size; i++) {
        HashNode* current = table->buckets[i];
        while (current != NULL) {
            HashNode* temp = current;
            current = current->next;
            free(temp);
        }
    }
    free(table->buckets);
    free(table);
}

// ===================================================================
// 3. 라벨 인코더 구현
// ===================================================================
typedef struct {
    char** labels;
    int num_labels;
} LabelEncoder;

LabelEncoder* load_label_encoder(const char* filename) {
    FILE* fp = fopen(filename, "r");
    if (!fp) {
        perror("Label encoder file open failed");
        return NULL;
    }
    
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char* buffer = (char*)malloc(file_size + 1);
    if (!buffer) {
        perror("Failed to allocate buffer for label encoder file");
        fclose(fp);
        return NULL;
    }
    fread(buffer, 1, file_size, fp);
    buffer[file_size] = '\0';
    fclose(fp);

    LabelEncoder* encoder = (LabelEncoder*)malloc(sizeof(LabelEncoder));
    if (!encoder) {
        perror("Failed to allocate LabelEncoder");
        free(buffer);
        return NULL;
    }
    encoder->labels = NULL;
    encoder->num_labels = 0;

    int capacity = 10;
    encoder->labels = (char**)malloc(capacity * sizeof(char*));
    if (!encoder->labels) {
        perror("Failed to allocate labels array");
        free(buffer);
        free(encoder);
        return NULL;
    }

    char* ptr = buffer;
    while(*ptr && *ptr != '[') ptr++;
    if(*ptr == '[') ptr++;

    while(*ptr) {
        while(*ptr && (*ptr == ' ' || *ptr == '\n' || *ptr == '\r' || *ptr == '\t')) ptr++;
        if(*ptr == ']') break;

        if(*ptr == '"') {
            ptr++;
            char* start = ptr;
            while(*ptr && *ptr != '"') ptr++;
            if(*ptr == '"') {
                int len = ptr - start;
                if (encoder->num_labels >= capacity) {
                    capacity *= 2;
                    char** new_labels = (char**)realloc(encoder->labels, capacity * sizeof(char*));
                    if (!new_labels) {
                        perror("Failed to realloc labels array");
                        for (int i = 0; i < encoder->num_labels; i++) free(encoder->labels[i]);
                        free(encoder->labels);
                        free(encoder);
                        free(buffer);
                        return NULL;
                    }
                    encoder->labels = new_labels;
                }
                encoder->labels[encoder->num_labels] = (char*)malloc(len + 1);
                if (!encoder->labels[encoder->num_labels]) {
                     perror("Failed to allocate for label string");
                     for (int i = 0; i < encoder->num_labels; i++) free(encoder->labels[i]);
                     free(encoder->labels);
                     free(encoder);
                     free(buffer);
                     return NULL;
                }
                strncpy(encoder->labels[encoder->num_labels], start, len);
                encoder->labels[encoder->num_labels][len] = '\0';
                encoder->num_labels++;
                ptr++;
            }
        }
        while(*ptr && *ptr != ',' && *ptr != ']') ptr++;
        if(*ptr == ',') ptr++;
    }

    free(buffer);
    return encoder;
}

const char* inverse_transform(LabelEncoder* encoder, int prediction) {
    if (encoder && prediction >= 0 && prediction < encoder->num_labels) {
        return encoder->labels[prediction];
    }
    return "Unknown";
}

void free_label_encoder(LabelEncoder* encoder) {
    if (encoder) {
        for (int i = 0; i < encoder->num_labels; i++) {
            free(encoder->labels[i]);
        }
        free(encoder->labels);
        free(encoder);
    }
}

// ===================================================================
// 4. 점진적 특징 추출 함수
// ===================================================================
TraceFeatures extract_features_incremental(char** lines, int start_line, int end_line) {
    TraceFeatures features = {0};
    
    if (start_line >= end_line) return features;
    
    HashTable* lba_table = create_hash_table(10000);
    
    long total_accesses = 0;
    long read_count = 0;
    long rw_switches = 0;
    long sequential_accesses = 0;

    char last_op = '\0', current_op;
    long last_lba = -1, current_lba;

    double* reuse_distances = NULL;
    size_t reuse_capacity = 1024;
    size_t reuse_count = 0;
    reuse_distances = (double*)malloc(reuse_capacity * sizeof(double));
    if (!reuse_distances) {
        free_hash_table(lba_table);
        return features;
    }

    int current_index = 0;

    for (int line_idx = start_line; line_idx < end_line; line_idx++) {
        if (sscanf(lines[line_idx], "%ld %c", &current_lba, &current_op) != 2) {
            continue;
        }
        total_accesses++;
        
        if (current_op == 'R' || current_op == 'r') {
            read_count++;
        }

        if (last_op != '\0' && last_op != current_op) {
            rw_switches++;
        }
        last_op = current_op;

        if (last_lba != -1 && current_lba == last_lba + 1) {
            sequential_accesses++;
        }
        
        HashNode* node = find_hash_node(lba_table, current_lba);
        if (node == NULL) {
            insert_hash_node(lba_table, current_lba, current_index); 
        } else {
            if (reuse_count >= reuse_capacity) {
                reuse_capacity *= 2;
                double* new_rd = (double*)realloc(reuse_distances, reuse_capacity * sizeof(double));
                if (!new_rd) {
                    free(reuse_distances);
                    free_hash_table(lba_table);
                    return features;
                }
                reuse_distances = new_rd;
            }
            reuse_distances[reuse_count++] = (double)(current_index - node->last_index);
            node->last_index = current_index;
            node->count++;
        }
        last_lba = current_lba;
        current_index++;
    }

    // 특징 계산
    if (total_accesses > 0) {
        features.read_ratio = (double)read_count / total_accesses;
        features.rw_switch_rate = (double)rw_switches / total_accesses;
        features.seq_access_ratio = (double)sequential_accesses / total_accesses;
        features.unique_address_ratio = (double)lba_table->item_count / total_accesses;
        features.access_locality = 1.0 - features.unique_address_ratio;
    }

    // Reuse Distance 계산
    if (reuse_count > 0) {
        double sum_reuse = 0;
        double max_reuse = 0;
        for (size_t i = 0; i < reuse_count; i++) {
            sum_reuse += reuse_distances[i];
            if (reuse_distances[i] > max_reuse) {
                max_reuse = reuse_distances[i];
            }
        }
        features.avg_reuse_distance = sum_reuse / reuse_count;
        features.max_reuse_distance = max_reuse;
    }

    // Entropy 계산
    if (total_accesses > 0 && lba_table->item_count > 0) {
        double entropy_val = 0;
        for (int i = 0; i < lba_table->size; i++) {
            HashNode* current_node = lba_table->buckets[i];
            while (current_node != NULL) {
                double p_x = (double)current_node->count / total_accesses;
                if (p_x > 0) {
                    entropy_val -= p_x * log2(p_x);
                }
                current_node = current_node->next;
            }
        }
        features.entropy = entropy_val;
    }
    
    if (reuse_distances) free(reuse_distances);
    free_hash_table(lba_table);
    return features;
}

// ===================================================================
// 5. 예측 함수
// ===================================================================
int predict_policy(TraceFeatures features, BoosterHandle booster) {
    float feature_array[8] = {
        (float)features.read_ratio,
        (float)features.avg_reuse_distance,
        (float)features.max_reuse_distance,
        (float)features.access_locality,
        (float)features.unique_address_ratio,
        (float)features.entropy,
        (float)features.rw_switch_rate,
        (float)features.seq_access_ratio
    };

    DMatrixHandle dmat;
    safe_xgboost(XGDMatrixCreateFromMat(feature_array, 1, 8, NAN, &dmat)); 

    char const config[] = "{\"training\": false, \"type\": 0, "
                         "\"iteration_begin\": 0, \"iteration_end\": 0, \"strict_shape\": false}";
    
    uint64_t const* out_shape = NULL;
    uint64_t out_dim = 0;
    float const* out_result = NULL;

    safe_xgboost(XGBoosterPredictFromDMatrix(booster, dmat, config, &out_shape, &out_dim, &out_result));
    
    int predicted_class = 0;
    float max_prob = 0.0f;
    uint64_t num_classes = out_shape[1];

    if (num_classes > 0) {
        max_prob = out_result[0];
        for (uint64_t i = 1; i < num_classes; i++) {
            if (out_result[i] > max_prob) {
                max_prob = out_result[i];
                predicted_class = (int)i;
            }
        }
    }
    
    safe_xgboost(XGDMatrixFree(dmat));
    return predicted_class;
}

// ===================================================================
// 6. 메인 함수
// ===================================================================
int main(int argc, char *argv[]) {
    const char* trace_file = "detailed_zns_trace.txt";
    const char* model_file = "xgb_model.json";
    const char* encoder_file = "label_encoder.json";
    const char* output_file = "output_workload.txt";

    if (argc > 1) trace_file = argv[1];
    if (argc > 3) model_file = argv[3];
    if (argc > 4) encoder_file = argv[4];
    if (argc > 2) output_file = argv[2];
    
    printf("Using trace file: %s\n", trace_file);
    printf("Using model file: %s\n", model_file);
    printf("Using encoder file: %s\n", encoder_file);
    printf("Output file: %s\n", output_file);

    // 파일 존재 확인
    FILE* test_fp = fopen(trace_file, "r");
    if (!test_fp) {
        printf("Input trace file does not exist: %s\n", trace_file);
        return 1;
    }
    fclose(test_fp);

    // 전체 파일을 메모리에 로드
    FILE* fp = fopen(trace_file, "r");
    if (!fp) {
        perror("Failed to open trace file");
        return 1;
    }

    // 파일 라인 수 계산
    char line_buffer[256];
    int total_lines = 0;
    while (fgets(line_buffer, sizeof(line_buffer), fp)) {
        total_lines++;
    }
    rewind(fp);

    // 모든 라인을 메모리에 저장
    char** lines = (char**)malloc(total_lines * sizeof(char*));
    if (!lines) {
        perror("Failed to allocate lines array");
        fclose(fp);
        return 1;
    }

    for (int i = 0; i < total_lines; i++) {
        if (fgets(line_buffer, sizeof(line_buffer), fp)) {
            int len = strlen(line_buffer);
            lines[i] = (char*)malloc(len + 1);
            strcpy(lines[i], line_buffer);
        }
    }
    fclose(fp);

    // XGBoost 모델 로딩
    printf("Loading XGBoost model...\n");
    BoosterHandle booster;
    safe_xgboost(XGBoosterCreate(NULL, 0, &booster));
    safe_xgboost(XGBoosterLoadModel(booster, model_file));

    // 라벨 인코더 로딩
    printf("Loading label encoder...\n");
    LabelEncoder* encoder = load_label_encoder(encoder_file);
    if (!encoder) {
        fprintf(stderr, "Failed to load label encoder.\n");
        return 1;
    }

    // 출력 파일 열기
    FILE* output_fp = fopen(output_file, "w");
    if (!output_fp) {
        perror("Failed to open output file");
        return 1;
    }

    printf("Starting incremental prediction...\n");
    
    int previous_policy = -1;
    int segment_size = total_lines / 10;  // 10등분
    
    for (int segment = 1; segment <= 10; segment++) {
        int end_line = segment * segment_size;
        if (segment == 10) end_line = total_lines;  // 마지막 세그먼트는 전체까지
        
        printf("Processing segment %d/10 (lines 0-%d)...\n", segment, end_line-1);
        
        // 현재 세그먼트까지의 특징 추출
        TraceFeatures features = extract_features_incremental(lines, 0, end_line);
        
        // 정책 예측
        int current_policy = predict_policy(features, booster);
        const char* policy_name = inverse_transform(encoder, current_policy);
        
        printf("Segment %d: Predicted policy = %s (Class %d)\n", segment, policy_name, current_policy);
        
        // 이전 세그먼트의 마지막 라인부터 현재 세그먼트까지 출력
        int start_output = (segment == 1) ? 0 : (segment - 1) * segment_size;
        
        for (int i = start_output; i < end_line; i++) {
            // 원본 라인 출력
            fputs(lines[i], output_fp);
            
            // 정책이 변경되었고, 세그먼트의 첫 번째 라인이라면 정책 표시 추가
            if (segment > 1 && i == start_output && current_policy != previous_policy) {
                fprintf(output_fp, "p %d\n", current_policy);
                printf("Policy changed at line %d: %s -> %s\n", 
                       i, 
                       (previous_policy >= 0) ? inverse_transform(encoder, previous_policy) : "None",
                       policy_name);
            }
        }
        
        // 첫 번째 세그먼트이거나 정책이 변경되지 않은 경우에도 세그먼트 마지막에 현재 정책 표시
        if (segment == 1 || (segment == 10 && current_policy != previous_policy)) {
            fprintf(output_fp, "p %d\n", current_policy);
        }
        
        previous_policy = current_policy;
    }

    fclose(output_fp);
    printf("Output written to %s\n", output_file);

    // 메모리 해제
    for (int i = 0; i < total_lines; i++) {
        free(lines[i]);
    }
    free(lines);
    safe_xgboost(XGBoosterFree(booster));
    free_label_encoder(encoder);

    printf("Incremental prediction completed.\n");
    return 0;
}
