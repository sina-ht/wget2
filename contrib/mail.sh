#!/bin/bash

set -e
set -o pipefail
set -u

FROM_NAME=
FROM_EMAIL=
TO_NAME=
TO_EMAIL=

get_from() {
	FROM_NAME="$(git config user.name)"
	FROM_EMAIL="$(git config user.email)"

	echo "The email will be sent as: $FROM_NAME <$FROM_EMAIL>"
	printf "Is this okay? [Y/n] "
	read -r ok
	case $ok in
		[yY]* ) return ;;
		"" ) return;;
		* ) exit 1 ;;
	esac
}

get_to() {
	printf "Name of recipient: "
	read -r TO_NAME
	printf "Email of recipient: "
	read -r TO_EMAIL
}

if [ ! -d "gnulib" ]; then
	echo "Error: Could not find the gnulib/ directory"
	exit 1
elif [ ! -f "contrib/assignment_template.txt" ]; then
	echo "Error: Could not find the assignment_template.txt file in contrib/"
	echo "Are you running this from the root of the git repository?"
	exit 1
fi

get_from
get_to
final_mail=$(mktemp)

if ! which msmtp >/dev/null 2>&1; then
	echo hallo
	echo "Could not find msmtp, you'll find the email in $final_mail"
fi

{
	echo "From: $FROM_NAME <$FROM_EMAIL>"
	echo "To: $TO_NAME <$TO_EMAIL>"
	echo "Cc: Darshit Shah <darnir@gnu.org>, Tim Rühsen <tim.ruehsen@gmx.de>"
	sed "s/%TO_NAME%/$TO_NAME/g" contrib/assignment_template.txt
	cat gnulib/doc/Copyright/request-assign.future
	echo "
--
Thanking You,
On Behalf of the maintainers of GNU Wget,
$FROM_NAME"
} > "$final_mail"

which msmtp 1>/dev/null 2>&1 && \
msmtp --add-missing-date-header -t < "$final_mail"
