#include <cstdio>
#include <cstring>
#include "detection.h"
#include "node_identity.h"
#include "gps_parse.h"
#include "focus_coverage.h"
#include "focus_plan.h"
static void sentence(const char *body, char *out) { unsigned c=0; for(auto p=body;*p;++p)c^=(unsigned char)*p; sprintf(out,"$%s*%02X",body,c); }
int main() {
 Detection d={}; d.raw_len=1; d.raw_packet[0]=0x42;
 char full[768]; size_t n=detectionFormatCsv(d,full,sizeof(full),"",false,0,0,0,1);
 char small[768]; memset(small,'X',sizeof(small));
 size_t got=detectionFormatCsv(d,small,n,"",false,0,0,0,1);
 printf("CSV exact short buffer: capacity=%zu returned=%zu sentinel=%u (expected X=88)\n",n,got,(unsigned char)small[n]);
 NodeIdentity id; id.node_id=1; strcpy(id.long_name,"=1+1"); char row[384];
 nodeIdentityFormatCsv(id,row,sizeof(row),"",false,0,0,0,1); printf("formula row: %s\n",row);
 GpsFix f; char s[160]; sentence("GPGGA,120000,4500.000,N,12200.000,W,1,08,1.0,0,M,0,M,,",s); gpsApplySentence(f,s,1000);
 sentence("GPGGA,120001,,,,,0,00,99.9,,,,,,",s); gpsApplySentence(f,s,2000);
 printf("no-fix GGA: quality=%u has_position=%u fresh=%u\n",f.fix_quality,f.has_position,gpsFixIsFresh(f,2000,10000));
 double coord=0; bool ok=nmeaCoordToDegrees("9900.000",'N',&coord); printf("invalid latitude accepted=%u value=%.1f\n",ok,coord);
 FocusCoverage c; focusCoverageNote(c,43,2000,2000,true); focusCoverageNote(c,43,2000,2000,true); focusCoverageNote(c,43,2000,2000,true);
 printf("same bin across regions cannot reset: US=%.3f Global=%.3f coverage=%s\n",energyBinFrequencyMhz(43,ENERGY_SWEEP_BAND_US,EnergyBinStep::KHZ_250),energyBinFrequencyMhz(43,ENERGY_SWEEP_BAND_GLOBAL,EnergyBinStep::KHZ_250),focusCoverageLabelName(focusCoverageLabelFor(c)));
 printf("Detection bytes=%zu\n",sizeof(Detection));
}
