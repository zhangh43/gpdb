#!/bin/bash -l

set -eox pipefail

./ccp_src/scripts/setup_ssh_to_cluster.sh
CLUSTER_NAME=$(cat ./cluster_env_files/terraform/name)

CWDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
source "${CWDIR}/common.bash"

function run_test(){
  local gpdb_master_alias=$1
  ssh $gpdb_master_alias bash -ex <<EOF
		trap look4diffs ERR

		function look4diffs() {

		    diff_files=\`find .. -name regression.diffs\`

		    for diff_file in \${diff_files}; do
			if [ -f "\${diff_file}" ]; then
			    cat <<-FEOF

						======================================================================
						DIFF FILE: \${diff_file}
						----------------------------------------------------------------------

						\$(cat "\${diff_file}")

					FEOF
			fi
		    done
		    exit 1
		}
		source /usr/local/greenplum-db-devel/greenplum_path.sh
		if [ -f /opt/gcc_env.sh ]; then
		    source /opt/gcc_env.sh
		fi

        export PGPORT=5432
        export MASTER_DATA_DIRECTORY=/data/gpdata/master/gpseg-1
        export LDFLAGS="-L\${GPHOME}/lib"
        export CPPFLAGS="-I\${GPHOME}/include"

        cd /home/gpadmin/gpdb_src
        ./configure --prefix=/usr/local/greenplum-db-devel \
            --without-zlib --without-rt --without-libcurl \
            --without-libedit-preferred --without-docdir --without-readline \
            --disable-gpcloud --disable-gpfdist --disable-orca \
            --disable-pxf ${CONFIGURE_FLAGS}

        make -C /home/gpadmin/gpdb_src/src/test/regress
		cd /home/gpadmin
		tar zcvf regress.tar.gz gpdb_src/src/test/regress/
		gpscp -f segment_host_list regress.tar.gz =:/home/gpadmin/
		gpscp -h smdw regress.tar.gz =:/home/gpadmin/
		gpssh -f segment_host_list 'tar zxvf regress.tar.gz'
		gpssh -h smdw 'tar zxvf regress.tar.gz'
		gpscp -f segment_host_list /usr/local/greenplum-db-devel/lib/postgresql/*.so =:/usr/local/greenplum-db-devel/lib/postgresql/
		gpscp -h smdw /usr/local/greenplum-db-devel/lib/postgresql/*.so =:/usr/local/greenplum-db-devel/lib/postgresql/
	
        cd /home/gpadmin/gpdb_src
		make -s ${MAKE_TEST_COMMAND}
EOF
}

function setup_gpadmin_user() {
    ./gpdb_src/concourse/scripts/setup_gpadmin_user.bash "$TEST_OS"
}

function _main() {
    if [ -z "${MAKE_TEST_COMMAND}" ]; then
        echo "FATAL: MAKE_TEST_COMMAND is not set"
        exit 1
    fi

    if [ -z "$TEST_OS" ]; then
        echo "FATAL: TEST_OS is not set"
        exit 1
    fi

    case "${TEST_OS}" in
    centos|ubuntu) ;; #Valid
    *)
      echo "FATAL: TEST_OS is set to an invalid value: $TEST_OS"
      echo "Configure TEST_OS to be centos, or ubuntu"
      exit 1
      ;;
    esac

    time run_test mdw

    if [ "${TEST_BINARY_SWAP}" == "true" ]; then
        time ./gpdb_src/concourse/scripts/test_binary_swap_gpdb.bash
    fi

    if [ "${DUMP_DB}" == "true" ]; then
        chmod 777 sqldump
        su gpadmin -c ./gpdb_src/concourse/scripts/dumpdb.bash
    fi
}

_main "$@"
