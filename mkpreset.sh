#!/bin/bash

## IFS=$(echo -en "\n\b")
## for file in *.wav; do FN=$(basename $file .wav); sndfile-convert -normalize -pcm24 $file ${FN}.flac ; done
## unset IFS

#UPFX="AWX_"
#NPFX="Airwindows - "

#UPFX="CKSDE_"
#NPFX="CKSDE - "

#UPFX="SCM7_"
#NPFX="M7 - "

#UPFX="BMT_"
#NPFX="Balance - Spaces - "

#UPFX="VOX_"
#NPFX="IMreverbs - Spaces - "

#UPFX="OAIR_"
#NPFX="OpenAir - Spaces - "

#UPFX="CCGB_"
#NPFX="Concertgebouw - "

#UPFX="JEZ_"
#NPFX="JezWells - Spaces - "

UPFX="LEX470L_"
NPFX="470L - "

FULLSTATE=false
FFTGAIN=`dirname $0`/tools/fftgain

test -x $FFTGAIN || exit

urlencode() {
	old_lc_collate=$LC_COLLATE
	LC_COLLATE=C

	local length="${#1}"
	for (( i = 0; i < length; i++ )); do
		local c="${1:i:1}"
		case $c in
			[a-zA-Z0-9.~_-]) printf "$c" ;;
			*) printf '%%%02X' "'$c" ;;
		esac
	done
	LC_COLLATE=$old_lc_collate
}

cat > manifest.ttl << EOF
@prefix atom: <http://lv2plug.in/ns/ext/atom#> .
@prefix lv2:  <http://lv2plug.in/ns/lv2core#>.
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#>.
@prefix pset: <http://lv2plug.in/ns/ext/presets#> .
@prefix zcpset: <http://gareus.org/oss/lv2/zeroconvolv/pset#> .

EOF

cat > presets.ttl << EOF
@prefix atom: <http://lv2plug.in/ns/ext/atom#> .
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix pset: <http://lv2plug.in/ns/ext/presets#> .
@prefix rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .
@prefix state: <http://lv2plug.in/ns/ext/state#> .
@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .
@prefix zcpset: <http://gareus.org/oss/lv2/zeroconvolv/pset#> .

EOF

IFS=$'\n'

for ir in *.flac; do
	FT=$(file "${ir}")
	BN=$(basename "${ir}" .flac | sed 's/_/ /g')
	if echo $FT | grep -q " stereo"; then
		if echo $BN | grep -q " ms"; then
			SFX="MonoToStereo"
		elif echo $BN | grep -q " m2s"; then
			SFX="MonoToStereo"
		else
			SFX="Stereo"
		fi
	elif echo $FT | grep -q " mono"; then
		SFX="Mono"
	elif echo $FT | grep -q " 4 channels"; then
		SFX="Stereo"
	else
		continue
	fi

	FILEURL=`urlencode "${ir}"`
	URISUFFIX=`echo -n "$BN" | sed 's/[^a-zA-Z0-9-]/_/g;s/___*/_/g'`

	case $BN in
		Ambiences*)
			COMMENT="The programs found in the Ambience group are reverberation programs designed add space without necessarily adding reverb. They are useful for a variety of tasks, and are especially well suited for post production and dialog applications, as well as adding “air” to voice and solo instruments."
			;;
		Effects*)
			COMMENT="The programs found in the Effects group are not necessarily adding reverb, but recreate miscellaneous audible artifacts. They are mainly useful for creating special effects."
			;;
		Halls*)
			COMMENT="The programs found in the Halls group are reverberation programs designed to emulate real concert halls and larger natural acoustical spaces. They are useful for a variety of tasks, and are especially well suited for orchestral music applications."
			;;
		Plates*)
			COMMENT="The programs found in the Plates group are reverberation programs designed to emulate the sound of an analog metal plate reverb. They are useful for a variety of tasks, and are especially well suited for popular multi-track recordings and live performance applications."
			;;
		Rooms*)
			COMMENT="The programs found in the Rooms group are reverberation programs designed to emulate rooms and smaller natural acoustical spaces. They are useful for a variety of tasks, and are especially well suited for post production applications and adding space to dialog and solo instruments in music mixes."
			;;
		Spaces*)
			COMMENT="The programs found in the Spaces group are reverberation programs designed to emulate outdoor sounds, non-real sounds, and other very large or small acoustical spaces. They are useful for a variety of tasks, and are especially well suited for post production creating special effects."
			;;
		*)
			COMMENT=""
			;;
	esac

	cat << EOF | tee -a manifest.ttl >> presets.ttl

zcpset:${UPFX}${URISUFFIX}
 a pset:Preset;
 lv2:appliesTo <http://gareus.org/oss/lv2/zeroconvolv#${SFX}>;
EOF

	echo " rdfs:seeAlso <presets.ttl>." >> manifest.ttl


	cat >> presets.ttl << EOF
 rdfs:label "${NPFX}${BN}";
 rdfs:comment "${COMMENT}";
 state:state [
  <http://gareus.org/oss/lv2/zeroconvolv#ir> <${FILEURL}>;
  <http://gareus.org/oss/lv2/zeroconvolv#predelay> "0"^^xsd:int ;
EOF

  $FFTGAIN "${ir}" >> presets.ttl

	if $FULLSTATE; then
	cat >> presets.ttl << EOF
  <http://gareus.org/oss/lv2/zeroconvolv#gain> "1.0"^^xsd:float ;
  <http://gareus.org/oss/lv2/zeroconvolv#sum_inputs> false;
  <http://gareus.org/oss/lv2/zeroconvolv#channel_predelay> [
    a atom:Vector ; atom:childType atom:Int ;
    rdf:value (
      "0"^^xsd:int
      "0"^^xsd:int
      "0"^^xsd:int
      "0"^^xsd:int
    )
  ] ;
  <http://gareus.org/oss/lv2/zeroconvolv#channel_gain> [
    a atom:Vector ; atom:childType atom:Float ;
    rdf:value (
      "1.0"^^xsd:float
      "1.0"^^xsd:float
      "1.0"^^xsd:float
      "1.0"^^xsd:float
    )
  ] ;
EOF
	fi

	cat >> presets.ttl << EOF
 ].
EOF

done
