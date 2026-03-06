/*
merge_logs.c
------------
Merge em C dos CSVs do DLG e do Drive por indice de linha.

Objetivo geral:
- Unificar logs parciais (dlg.csv + drive.csv) em resultado_ensaio.csv.
- Preservar indicadores de erro de cada origem (DLG e Drive) no arquivo final.
- Oferecer caminho rapido/compilado para merge no fim do ensaio.

Fluxo principal:
1) Parse de argumentos --dlg, --drive e --out.
2) Abertura dos 2 arquivos de entrada e do arquivo de saida.
3) Leitura linha a linha, alinhando pelo indice (com fallback incremental).
4) Escrita da linha consolidada com colunas fixas de resultado.
5) Gravacao de assinatura "<out>.merge_source.txt" para rastreabilidade.

Resumo de funcoes:
- trim: remove espacos/quebras para parse robusto.
- split_csv: separa colunas por virgula sem alocacao dinamica.
- print_usage: mensagem de uso da CLI.
- write_merge_signature: grava marcador textual da origem do merge.
- main: orquestra todo o merge e valida erros de IO.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trim(char *s){
    size_t n = strlen(s);
    while(n && (s[n-1]=='\n'||s[n-1]=='\r'||s[n-1]==' '||s[n-1]=='\t')) s[--n]=0;
    char *p = s; while(*p == ' ' || *p == '\t') p++;
    if(p != s) memmove(s, p, strlen(p)+1);
}

static int split_csv(char *line, char **cols, int max_cols){
    int n = 0;
    char *p = line;
    while(n < max_cols && p){
        cols[n++] = p;
        p = strchr(p, ',');
        if(p){
            *p = '\0';
            p++;
        }
    }
    return n;
}

static void print_usage(void){
    puts("Uso:");
    puts("  merge_logs --dlg <dlg.csv> --drive <drive.csv> --out <merged.csv>");
}

static void write_merge_signature(const char *out_path, const char *source){
    char sig_path[2048];
    _snprintf(sig_path, sizeof(sig_path), "%s.merge_source.txt", out_path);
    FILE *fsig = fopen(sig_path, "w");
    if(!fsig) return;
    fprintf(fsig, "merge_source=%s\n", source);
    fclose(fsig);
}

int main(int argc, char **argv){
    const char *dlg_path = NULL;
    const char *drive_path = NULL;
    const char *out_path = NULL;

    for(int i = 1; i < argc; ++i){
        if(strcmp(argv[i], "--dlg") == 0 && i + 1 < argc){ dlg_path = argv[++i]; continue; }
        if(strcmp(argv[i], "--drive") == 0 && i + 1 < argc){ drive_path = argv[++i]; continue; }
        if(strcmp(argv[i], "--out") == 0 && i + 1 < argc){ out_path = argv[++i]; continue; }
        print_usage();
        return 1;
    }
    if(!dlg_path || !drive_path || !out_path){
        print_usage();
        return 1;
    }

    FILE *fdlg = fopen(dlg_path, "r");
    FILE *fdrv = fopen(drive_path, "r");
    FILE *fout = fopen(out_path, "w");
    if(!fdlg || !fdrv || !fout){
        fprintf(stderr, "Falha abrindo arquivos.\n");
        if(fdlg) fclose(fdlg);
        if(fdrv) fclose(fdrv);
        if(fout) fclose(fout);
        return 1;
    }

    /* skip headers */
    char line_dlg[2048];
    char line_drv[512];
    fgets(line_dlg, sizeof(line_dlg), fdlg);
    fgets(line_drv, sizeof(line_drv), fdrv);

    fprintf(fout, "idx,t_s,ch1,ch2,ch3,ch4,ch5,ch6,ch7,ch8,pos,rpm,dlg_err,drive_pos_err,drive_rpm_err\n");

    int idx_fallback = 0;
    for(;;){
        char *ld = fgets(line_dlg, sizeof(line_dlg), fdlg);
        char *lr = fgets(line_drv, sizeof(line_drv), fdrv);
        if(!ld && !lr) break;

        char *dlg_cols[16] = {0};
        char *drv_cols[12] = {0};
        int dlg_n = 0, drv_n = 0;

        if(ld){
            trim(line_dlg);
            dlg_n = split_csv(line_dlg, dlg_cols, 16);
        }
        if(lr){
            trim(line_drv);
            drv_n = split_csv(line_drv, drv_cols, 12);
        }

        /* DLG expected: idx,t_qpc,t_s,ch1..ch8,err */
        const char *idx = (dlg_n >= 1 && dlg_cols[0][0]) ? dlg_cols[0]
                         : (drv_n >= 1 && drv_cols[0][0]) ? drv_cols[0]
                         : NULL;
        char idx_buf[32];
        if(!idx){
            _snprintf(idx_buf, sizeof(idx_buf), "%d", idx_fallback);
            idx = idx_buf;
        }

        const char *t_s = (dlg_n >= 3 && dlg_cols[2][0]) ? dlg_cols[2]
                         : (drv_n >= 3 && drv_cols[2][0]) ? drv_cols[2]
                         : "NULL";

        const char *ch[8];
        for(int i = 0; i < 8; ++i){
            int col = 3 + i; /* ch1 starts at 4th col */
            ch[i] = (dlg_n > col && dlg_cols[col][0]) ? dlg_cols[col] : "NULL";
        }

        const char *pos = (drv_n >= 4 && drv_cols[3][0]) ? drv_cols[3] : "NULL";
        const char *rpm = (drv_n >= 5 && drv_cols[4][0]) ? drv_cols[4] : "NULL";
        const char *dlg_err = (dlg_n >= 12 && dlg_cols[11][0]) ? dlg_cols[11] : "1";
        const char *drv_pos_err = (drv_n >= 6 && drv_cols[5][0]) ? drv_cols[5] : "1";
        const char *drv_rpm_err = (drv_n >= 7 && drv_cols[6][0]) ? drv_cols[6] : "1";

        fprintf(fout, "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                idx, t_s,
                ch[0], ch[1], ch[2], ch[3], ch[4], ch[5], ch[6], ch[7],
                pos, rpm, dlg_err, drv_pos_err, drv_rpm_err);

        idx_fallback++;
    }

    fclose(fdlg);
    fclose(fdrv);
    fclose(fout);
    write_merge_signature(out_path, "merge_logs_c");
    return 0;
}
