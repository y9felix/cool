#!/usr/bin/env bash
set -u

MUSIC_DIR="$HOME/music"
LINKS="$MUSIC_DIR/links.txt"
FINISHED="$MUSIC_DIR/finished.txt"
FAILED="$MUSIC_DIR/failed.txt"
STATE="$MUSIC_DIR/.current"

PAUSE_403="${PAUSE_403:-1800}"
RETRY_PAUSE="${RETRY_PAUSE:-60}"
MAX_FAILS="${MAX_FAILS:-5}"
PID=""

mkdir -p "$MUSIC_DIR"
touch "$LINKS" "$FINISHED"

log() { printf '[%s] %s\n' "$(date '+%F %T')" "$*"; }

pick_link() {
    sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e '/^$/d' "$LINKS" | shuf -n1
}

remove_link() {
    local tmp
    tmp=$(mktemp "$MUSIC_DIR/.tmp.XXXXXX")
    LINK="$1" awk '{ s=$0; gsub(/^[[:space:]]+|[[:space:]]+$/, "", s); if (s != ENVIRON["LINK"]) print }' "$LINKS" > "$tmp" && mv "$tmp" "$LINKS"
}

save_state() { printf '%s\t%s\n' "$1" "$2" > "$STATE"; }

cleanup() {
    log "Interrupted - state kept in $STATE"
    [[ -n $PID ]] && kill "$PID" 2>/dev/null
    exit 130
}
trap cleanup INT TERM

run_download() {
    local link=$1 start=$2 line rc fifo fd
    local re='\[download\] Downloading item ([0-9]+) of ([0-9]+)'
    local args=(-x --audio-format mp3 --audio-quality 0 --embed-thumbnail --embed-metadata
                --newline -o "$MUSIC_DIR/%(uploader)s/%(title)s.%(ext)s")
    (( start > 1 )) && args+=(--playlist-start "$start")

    RESULT="error"
    fifo=$(mktemp -u) && mkfifo "$fifo"
    yt-dlp "${args[@]}" "$link" > "$fifo" 2>&1 &
    PID=$!
    exec {fd}< "$fifo"
    rm -f "$fifo"

    while IFS= read -r line <&"$fd"; do
        printf '%s\n' "$line"
        if [[ $line =~ $re ]]; then
            INDEX=${BASH_REMATCH[1]}
            save_state "$link" "$INDEX"
        elif [[ $line == *"[download] Finished downloading playlist:"* ]]; then
            RESULT="finished"
        elif [[ $line == *"HTTP Error 403"* ]]; then
            RESULT="403"
            kill "$PID" 2>/dev/null
            break
        fi
    done

    exec {fd}<&-
    wait "$PID"; rc=$?
    PID=""
    [[ $RESULT == "error" && $rc -eq 0 ]] && RESULT="finished"
}

process_link() {
    local link=$1 fails=0 prev_index
    INDEX=$2

    while true; do
        prev_index=$INDEX
        log "Downloading: $link (starting at item $INDEX)"
        run_download "$link" "$INDEX"

        case $RESULT in
            finished)
                log "Finished: $link"
                printf '%s\n' "$link" >> "$FINISHED"
                remove_link "$link"
                rm -f "$STATE"
                return
                ;;
            403)
                log "HTTP 403 - freezing for $((PAUSE_403 / 60)) min, then retrying item $INDEX"
                sleep "$PAUSE_403"
                ;;
            *)
                if [[ $INDEX == "$prev_index" ]]; then
                    (( fails++ ))
                else
                    fails=0
                fi
                if (( fails >= MAX_FAILS )); then
                    log "Giving up on $link after $fails failures -> $FAILED"
                    printf '%s\n' "$link" >> "$FAILED"
                    remove_link "$link"
                    rm -f "$STATE"
                    return
                fi
                log "yt-dlp stopped unexpectedly (failure $fails/$MAX_FAILS), retrying in ${RETRY_PAUSE}s"
                sleep "$RETRY_PAUSE"
                ;;
        esac
    done
}

while true; do
    if [[ -s $STATE ]]; then
        IFS=$'\t' read -r link index < "$STATE"
        log "Resuming unfinished link at item ${index:=1}"
    else
        link=$(pick_link)
        [[ -z $link ]] && { log "links.txt is empty - nothing left to do"; exit 0; }
        index=1
        save_state "$link" 1
    fi
    process_link "$link" "$index"
done
