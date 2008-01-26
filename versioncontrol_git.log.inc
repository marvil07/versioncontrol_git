<?php
// $Id: versioncontrol_git.log.inc,v 1.1 2008/01/26 21:31:44 boombatower Exp $
/**
 * @file
 * Git backend for Version Control API - Provides Git commit information and
 * account management as a pluggable backend.
 *
 * Copyright 2008 by Jimmy Berry ("boombatower", http://drupal.org/user/214218)
 */

/**
 * Actually update the repository by fetching commits and other stuff
 * directly from the repository, invoking the git executable.
 *
 * @return
 *   TRUE if the logs were updated, or FALSE if fetching and updating the logs
 *   failed for whatever reason.
 */
function _versioncontrol_git_log_update_repository(&$repository) {
  $root = escapeshellcmd($repository['root']);

  // Prepare the "since when" time argument for Git, and remember the
  // current time for the "updated" value that is stored later on. Fri Jan 25 18:00:44 2008 -0600
  $date = ($repository['git_specific']['updated'] > 0)
          ? '--since="'. gmdate('D M j G:i:s Y O', $repository['git_specific']['updated']) .'"'
          : '';
  $date_updated = time();

  $file_revisions = array();

  // Call Git in order to get the raw logs.
  $command = "cd $root;"."git log --numstat --summary --pretty=medium $date";
  if (variable_get('versioncontrol_git_log_use_file', 1)) {
    $temp_dir = variable_get('file_directory_temp', (PHP_OS == 'WINNT' ? 'c:\\windows\\temp' : '/tmp'));
    $temp_file = $temp_dir .'/git-'. rand();
    exec($command . " > $temp_file");
    $logs = fopen($temp_file, 'r');
  }
  else {
    exec($command, $logs); // TODO match above
    reset($logs); // reset the array pointer, so that we can use next()
  }
  watchdog('special', $command);

  // Parse the info from the raw output.
  _versioncontrol_git_log_parse($repository, $logs, $file_revisions);

  if (variable_get('versioncontrol_git_log_use_file', 1)) {
    fclose($logs);
    unlink($temp_file);
  }

  // Having retrieved the file revisions, insert those into the database
  // as Version Control API commits.
  _versioncontrol_git_log_process($repository, $file_revisions);
  $repository['git_specific']['updated'] = $date_updated;

  // Everything's done, remember the time when we updated the log (= now).
  db_query('UPDATE {versioncontrol_git_repositories}
            SET updated = %d WHERE repo_id = %d',
            $repository['git_specific']['updated'], $repository['repo_id']);
  return TRUE;
}

/**
 * Parse the logs into a list of file revision objects, so that they
 * can be processed more easily.
 *
 * @param $repository
 *   The repository array, as given by the Version Control API.
 * @param $logs
 *   Either an array containing all the output lines (if the output was
 *   directly read by exec()) or a file handle of the temporary file
 *   that the output was written to.
 * @param $file_revisions
 *   An array that will be filled with a simple, flat list of
 *   file revision objects. Each object has the following properties:
 *
 *   - revision: The revision number (a string, e.g. '1.1' or '1.59.2.3').
 *   - date: The time of the revision, as Unix timestamp.
 *   - username: The Git username of the committer.
 *   - dead: TRUE if the file revision is in the "dead" (deleted) state,
 *       or FALSE if it currently exists in the repository.
 *   - lines_added: An integer that specifies how many lines have been added
 *       in this revision.
 *   - lines_removed: An integer that specifies how many lines have been added
 *       in this revision.
 *   - commitid: Optional property, may exist in more recent versions of Git.
 *       (It seems to have been introduced in 2005 or something.) If given,
 *       this is a string which is the same for all file revisions in a commit.
 *   - message: The commit message (a string with possible line breaks).
 *   - branch: The branch that this file revision was committed to,
 *       as string containing the name of the branch.
 */
function _versioncontrol_git_log_parse($repository, &$logs, &$file_revisions) {
  // If the log was retrieved by taking the return value of exec(), we've
  // got an array and navigate it via next(). If we stored the log in a
  // temporary file, $logs is a file handle that we need to fgets() instead.
  $next = is_array($logs) ? 'next' : 'fgets';

  $root_path = $repository['root'];

  while (($line = $next($logs)) !== FALSE) {
    // Revision info.
    $matches_found = preg_match('/^commit (.+)$/', $line, $matches);
    if (!$matches_found) {
      continue;
    }
    $commit_id = $matches[1];

    $line = $next($logs);
    if (preg_match('/^Author: ([^<]+)/', $line, $matches)) { // Ignore e-mail address.
      $author = trim($matches[1]);
    }

    $line = $next($logs);
    if (preg_match('/^Date: (.+)$/', $line, $matches)) {
      $date = trim($matches[1]);
    }

    // Get revision message.
    $next($logs); // Blank line.
    $message = '';
    while (($line = $next($logs)) !== FALSE) {
      if (trim($line) != '') {
        $message .= trim($line);
      }
      else {
        break;
      }
    }

    // Read file line revisions.
    $revisions = array();
    while (($line = $next($logs)) !== FALSE) {
      if (is_numeric($line[0]) && preg_match('/^(\S+)'."\t".'(\S+)'."\t".'(.+)$/', $line, $matches)) { // Begins with num lines added and matches expression.
        $file_revision = new StdClass();
        $file_revision->commit_id = $commit_id;
        $file_revision->username = $author;
        $file_revision->date = strtotime($date);
        $file_revision->message = $message;
        $file_revision->lines_added = $matches[1];
        $file_revision->lines_removed = $matches[2];
        $file_revision->path = '/'. $matches[3];
        $file_revision->action = VERSIONCONTROL_ACTION_MODIFIED;
        $revisions[] = $file_revision;
      }
      else {
        break;
      }
    }

    // Read file actions.
    $i = 0;
    do {
      if (preg_match('/^ (\S+) (\S+) (\S+) (.+)$/', $line, $matches)) { // Ensure that same file, they should be in same order.
        $revisions[$i]->action = ($matches[1] == 'create' ? VERSIONCONTROL_ACTION_ADDED : VERSIONCONTROL_ACTION_DELETED);
        $i++;
      }
      else {
        break;
      }
    }
    while (($line = $next($logs)) !== FALSE);

    $file_revisions = array_merge($file_revisions, $revisions);
  } // Loop to the next revision.
}

/**
 * Update the database by processing and inserting the previously retrieved
 * file revision objects.
 *
 * @param $repository
 *   The repository array, as given by the Version Control API.
 * @param $file_revisions
 *   A simple, flat list of file revision objects - the combined set of
 *   return values from _versioncontrol_git_log_parse().
 */
function _versioncontrol_git_log_process($repository, $file_revisions) {
  $commit_actions = array();

  foreach ($file_revisions as $file_revision) {
    // Don't insert the same revision twice.
    $count = db_result(db_query(
      "SELECT COUNT(*)
       FROM {versioncontrol_git_item_revisions} ir
        INNER JOIN {versioncontrol_operations} op
         ON ir.vc_op_id = op.vc_op_id
       WHERE op.repo_id = %d AND op.type = %d
        AND ir.path = '%s' AND ir.commit_id = '%s'",
      $repository['repo_id'], VERSIONCONTROL_OPERATION_COMMIT,
      $file_revision->path, $file_revision->commit_id
    ));
    if ($count > 0) {
      continue;
    }

    // We might only pick one of those (depending if the file
    // has been added, modified or deleted) but let's add both
    // current and source items for now.
    $commit_action = array(
      'action' => VERSIONCONTROL_ACTION_MODIFIED, // default, might be changed
      'current item' => array(
        'type' => VERSIONCONTROL_ITEM_FILE,
        'path' => $file_revision->path,
        'commit_id' => $file_revision->commit_id,
      ),
      'source items' => array(
        array(
          'type' => VERSIONCONTROL_ITEM_FILE,
          'path' => $file_revision->path,
          'commit_id' => versioncontrol_git_get_previous_commit_id($repository['repo_id'], VERSIONCONTROL_OPERATION_COMMIT, $file_revision->path),
        ),
      ),
      'git_specific' => array(
        'file_revision' => $file_revision, // Temporary.
        'lines_added' => $file_revision->lines_added,
        'lines_removed' => $file_revision->lines_removed,
        'commit_id' => $file_revision->commit_id,
      ),
    );

    // Clean up $commit_action based on the action being performed.
    if ($file_revision->action == VERSIONCONTROL_ACTION_DELETED) {
      $commit_action['action'] = VERSIONCONTROL_ACTION_DELETED;
      unset($commit_action['current item']);
    }
    else if ($file_revision->action == VERSIONCONTROL_ACTION_ADDED) {
      $commit_action['action'] = VERSIONCONTROL_ACTION_ADDED;
      unset($commit_action['source items']);
    }

    $commit_actions[$file_revision->commit_id][$file_revision->path] = $commit_action;
  }

  $commits = array();
  foreach ($commit_actions as $commit_id => $commit_actions) {
    _versioncontrol_git_log_construct_commit($repository, $commit_actions, $commits);
  }

  // Ok, we've got all commits gathered and in a nice array with
  // the commit date as key. So the only thing that's left is to sort them
  // and then send each commit to the API function for inserting into the db.
  ksort($commits);
  foreach ($commits as $date => $date_commits) {
    foreach ($date_commits as $commit_info) {
      versioncontrol_insert_commit($commit_info->commit, $commit_info->commit_actions);
    }
  }
}

/**
 * Get the previous commit id of file under version control,
 * given the repository id, path, and operation id.
 * 
 * @return string Previous commit id.
 */
function versioncontrol_git_get_previous_commit_id($repo_id, $path, $op_id) {
  $result = db_result(db_query(
    "SELECT ir.commit_id
     FROM {versioncontrol_git_item_revisions} ir
      INNER JOIN {versioncontrol_operations} op
       ON ir.vc_op_id = op.vc_op_id
     WHERE op.repo_id = %d AND ir.path = '%s'
     AND op.date < (SELECT date FROM {versioncontrol_operations} WHERE vc_op_id = %d)
     ORDER BY op.date DESC
     LIMIT 1",
    $repo_id, $path, $op_id
  ));
  return ($result !== FALSE ? $result : NULL);
}

/**
 * Use the additional file revision information that has been stored
 * in each commit action array in order to assemble the associated commit.
 * That commit information is then stored as a list item in the given
 * $commits array as an object with 'commit' and 'commit_actions' properties.
 */
function _versioncontrol_git_log_construct_commit($repository, $commit_actions, &$commits) {
  $date = 0;

  // Get any of those commit properties, apart from the date (which may
  // vary in large commits) they should all be the same anyways.
  foreach ($commit_actions as $path => $commit_action) {
    $file_revision = $commit_action['git_specific']['file_revision'];
    unset($commit_actions[$path]['git_specific']['file_revision']);

    if ($file_revision->date > $date) {
      $date = $file_revision->date;
    }
    $username = $file_revision->username;
    $message = $file_revision->message;
    $branch_name = 'master'; // $file_revision->branch;
  }

  // Get the branch id, and insert the branch into the database
  // if it doesn't exist yet.
  $branch_id = versioncontrol_ensure_branch($branch_name, $repository['repo_id']);

  // Yay, we have all commit actions and all information. Ready to go!
  $commit = array(
    'repo_id' => $repository['repo_id'],
    'date' => $date,
    'username' => $username,
    'message' => $message,
    'revision' => '',
    'git_specific' => array(
      'branch_id' => $branch_id,
    ),
  );

  $commit_info = new StdClass();
  $commit_info->commit = $commit;
  $commit_info->commit_actions = $commit_actions;
  $commits[$date][] = $commit_info;
}
