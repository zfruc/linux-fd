set -e

startTime=`date +%Y%m%d-%H:%M`
startTime_s=`date +%s`
#make bzImage
#make menuconfig
#make modules -j8
#make modules_install
#make
#make install
make
make install
#make modules_install install
endTime=`date +%Y%m%d-%H:%M`
endTime_s=`date +%s`
sumTime=$[ $endTime_s - $startTime_s ]
useTime=$[ $sumTime / 60 ]
echo "$startTime ---> $endTime" "Totl:$useTime minutes"
