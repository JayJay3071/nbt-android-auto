#!/bin/sh
# Read-only: how the stock media management (mcd, mme, qdb) finds and describes media sources. Text only, changes nothing.
# NEVER add /dev/io-amp/routing|debug|diagnosis|processing here (reading them restarted the head unit).
# Send with: python .local\nbt_request.py "tr -d '\015' | sh" --input-file scripts\car-mme-probe.sh --timeout 110 --output-file <file>
L=/opt/mm/lib:/opt/sys/lib:/opt/share/lib:/lib:/usr/lib
echo "##### `date`"
echo '##### mcd.cfg'; grep -v -e '^ *#' -e '^ *$' /etc/mcd.cfg
echo '##### mme.conf'; grep -v -e '^ *#' -e '^ *$' -e '^ *<!--' /opt/mm/etc/mme.conf | sed 420q
echo '##### sql dir'; ls -l /opt/mm/etc/sql /opt/mm/etc/update
echo '##### qdb.cfg'; grep -v -e '^ *#' -e '^ *$' /opt/mm/etc/qdb.cfg
echo '##### qdb tools'; ls -l /usr/bin/qdbc /bin/qdbc /usr/sbin/qdbc /opt/mm/bin/qdbc /opt/sys/bin/qdbc /opt/share/bin 2>&1 | sed 60q
echo '##### mmecli help'; LD_LIBRARY_PATH=$L /opt/mm/bin/mmecli -c /dev/mme/multichannel help 2>&1 | sed 150q
echo '##### mmecli status'; for c in get_status get_play_state get_now_playing mediastores slots; do echo "--- $c"; LD_LIBRARY_PATH=$L /opt/mm/bin/mmecli -c /dev/mme/multichannel $c 2>&1 | sed 30q; done
echo '##### devices'; ls -l /dev/IAP* /dev/umass* /dev/ser* /dev/oap* /dev/usb* 2>&1 | sed 30q; ls /fs /mnt
echo '##### ipod.cfg'; grep -v -e '^ *#' -e '^ *$' /opt/mm/etc/ipod.cfg | sed 60q
echo '##### mmlauncher cfg'; grep -v -e '^ *#' -e '^ *$' /opt/mm/etc/mmlauncher_B069.cfg | sed 120q
echo '##### prepare script'; cat /opt/mm/bin/prepare_and_start_up_media.sh
echo '##### music collection'; ls /mnt/quota/mm | sed 20q; ls /mnt/quota/mm/EntServer | sed 12q
echo '##### end'
