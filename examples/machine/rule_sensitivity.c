/* Does the rule baseline survive a perturbation of its own thresholds?
 *
 * #65 asked for it and #77 needs the answer: rules and scenarios in this
 * roadmap share an author, so "9 of 9" is only worth something if it does not
 * sit exactly on the numbers that were chosen for it.
 *
 * Build:
 *   clang -std=c23 -Iinclude -Iinclude/geistshell -o /tmp/sens \
 *     examples/machine/rule_sensitivity.c build/host-debug/lib/libgeistshell.a \
 *     deps/geist/lib/<target>/libgeist.a
 *
 * Result at the time of writing: 9/9 at default, 9/9 at -10%, 6/9 at +10%.
 * The memory cases sit at 95% and 96.6% against a 90% threshold, so raising it
 * by a tenth takes them out. */

#include "geistshell/diagnose.h"
#include "geistshell/machine_fixture.h"
#include <stdio.h>
#include <string.h>
static struct spg_sexpr_token tk[512]; static struct spg_sexpr_node nd[512];
static const char *files[] = {"1_batch_cpu","2_critical_cpu","3_memory_batch","4_thermal",
  "5_contradictory","6_healthy","7_heldout_thermal_batch","8_heldout_memory_critical","9_no_processes"};
static const char *want[] = {"batch_pressure","critical_pressure","memory_pressure","thermal_anomaly",
  "inconclusive","healthy","thermal_anomaly","memory_pressure","inconclusive"};
static int run(struct spg_rule_thresholds t, const char *label){
  int ok=0;
  for(size_t i=0;i<9;i++){
    char path[256]; snprintf(path,sizeof path,"examples/eval/machine/states/%s.spg",files[i]);
    FILE *f=fopen(path,"rb"); if(!f){printf("fehlt: %s\n",path); return -1;}
    char buf[8192]; size_t n=fread(buf,1,sizeof buf,f); fclose(f);
    struct spg_machine_state s={};
    if(spg_machine_state_parse(n,buf,512u,tk,512u,nd,&s)!=SPG_OK) continue;
    struct spg_diagnosis_result r={};
    if(spg_rule_diagnose(&s,&t,&r)!=SPG_OK) continue;
    if(strcmp(spg_diagnosis_to_string(r.diagnosis),want[i])==0) ok++;
  }
  printf("%-14s %d/9\n",label,ok); return ok;
}
int main(void){
  struct spg_rule_thresholds base=spg_rule_thresholds_default();
  run(base,"default");
  struct spg_rule_thresholds lo=base, hi=base;
  lo.cpu_high_bp=base.cpu_high_bp*9/10; lo.memory_high_bp=base.memory_high_bp*9/10;
  lo.temperature_high_mc=base.temperature_high_mc*9/10; lo.process_share_bp=base.process_share_bp*9/10;
  hi.cpu_high_bp=base.cpu_high_bp*11/10; hi.memory_high_bp=base.memory_high_bp*11/10;
  hi.temperature_high_mc=base.temperature_high_mc*11/10; hi.process_share_bp=base.process_share_bp*11/10;
  run(lo,"-10%"); run(hi,"+10%");
  return 0;}
