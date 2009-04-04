<?php
// $Id: versioncontrol_git.log.inc,v 1.27.2.1 2009/04/04 13:54:58 marvil07 Exp $
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
  chdir($root = escapeshellcmd($repository['root'])); // Set working directory to root.
  $latest_commits = _versioncontrol_git_get_lasest_commits($repository); // Get latest commits from database.
  $branches = _versioncontrol_git_log_get_branches(); // Get the list of current branches from Git.

  // Check for any branches that are no longer part of the repository.
  $obsolete_branches = array();
  foreach ($latest_commits as $branch => $latest_commit) {
    if (!in_array($branch, $branches)) {
      $obsolete_branches[] = $branch;
    }
  }

  // Check for new branches.
  foreach ($branches as $branch) {
    if (!array_key_exists($branch, $latest_commits)) {
      $latest_commits[$branch] = FALSE; // Add to list so it will be processed later.
    }
  }

  // Record new commits.
  $file_revisions = array();
  $previous_count = 0;
  foreach ($latest_commits as $branch => $latest_commit) {
    // Generate either '[hash]..' or '[branch]' or '[hash]..[branch]' as range specification for git log.
    $range = $latest_commit ? ($latest_commit .'..') : '';
    if (!in_array($branch, $obsolete_branches)) { // If the branch is obsolete then use it as the ending portion of the range.
      $range .= $branch;
    }

    // Get logs from Git.
    _versioncontrol_git_log_get_commits($repository, $file_revisions, $branch, $range);

    // If new revision set as latest revision.
    if (count($file_revisions) > $previous_count) {
      $latest_commits[$branch] = $file_revisions[$previous_count]->revision;
      $previous_count = count($file_revisions);
    }

    $branch_id = versioncontrol_git_get_label_id($repository['repo_id'], $branch, VERSIONCONTROL_OPERATION_BRANCH);

    if ($branch_id === FALSE) {
      db_query('DELETE FROM {versioncontrol_git_latest_commits} WHERE branch_id = %d', $branch_id);

      $lastest_commits_id = db_last_insert_id('{versioncontrol_git_latest_commits}_lastest_commits_id');
      db_query("INSERT INTO {versioncontrol_git_latest_commits} (lastest_commits_id, repo_id, branch_id, revision) VALUES (%d, %d, %d, '%s')",
        $lastest_commits_id, $repository['repo_id'], $branch_id, $latest_commits[$branch]);
    }
  }

  // Having retrieved the file revisions, insert those into the database
  // as Version Control API commits.
  _versioncontrol_git_log_process($repository, $file_revisions);

  // Delete obsolete branches.
  foreach ($obsolete_branches as $branch) {
    $branch_id = versioncontrol_get_branch_id($branch, $repository['repo_id']);

    if (!empty($branch_id)) {
      db_query('DELETE FROM {versioncontrol_git_latest_commits} WHERE branch_id = %d', $branch_id);
      db_query('DELETE FROM {versioncontrol_git_commits} WHERE branch_id = %d', $branch_id);
      versioncontrol_delete_branch($repository['repo_id'], $branch);
    }
  }

  // Check tags.
  $tags = _versioncontrol_git_log_get_tags();
  _versioncontrol_git_log_process_tags($repository, $tags);

  // Update repository updated field. Displayed on administration interface for documentation purposes.
  $repository['git_specific']['updated'] = time();
  db_query('UPDATE {versioncontrol_git_repositories}
            SET updated = %d WHERE repo_id = %d',
            $repository['git_specific']['updated'], $repository['repo_id']);
  return TRUE;
}

/**
 * Get an array containing branch name for key and the latest recorded commit
 * revision hash.
 */
function _versioncontrol_git_get_lasest_commits($repository) {
  $result = db_query('SELECT *
    FROM {versioncontrol_git_latest_commits} c
     INNER JOIN {versioncontrol_branches} b
      ON c.branch_id = b.branch_id
    WHERE b.repo_id = %d',
    $repository['repo_id']);

  $latest_commits = array();
  while ($latest_commit = db_fetch_object($result)) {
    $latest_commits[$latest_commit->branch_name] = $latest_commit->revision;
  }
  return $latest_commits;
}

/**
 * Execute a Git command using the root context and the command to be executed.
 * 
 * @param string $command Command to execute.
 * @param reference $temp_file Reference to temporaray file.
 * @return mixed Logged output from the command in either array of file pointer form.
 */
function _versioncontrol_git_log_exec($command, &$temp_file) {
  if (variable_get('versioncontrol_git_log_use_file', 1)) {
    $temp_dir = variable_get('file_directory_temp', (PHP_OS == 'WINNT' ? 'c:\\windows\\temp' : '/tmp'));
    $temp_file = $temp_dir .'/git-'. rand();
    exec("$command > $temp_file");
    $logs = fopen($temp_file, 'r');
  }
  else {
    $logs = array();
    exec($command, $logs);
    reset($logs); // Reset the array pointer, so that we can use next().
  }
  return $logs;
}

/**
 * Get branches from Git using 'branch -l' command.
 * 
 * @return array List of branches.
 */
function _versioncontrol_git_log_get_branches() {
  $logs = _versioncontrol_git_log_exec('git branch -l', $temp_file); // Query branches.
  $branches = _versioncontrol_git_log_parse_branches($logs); // Parse output.
  if (variable_get('versioncontrol_git_log_use_file', 1)) { // Close file.
    fclose($logs);
    unlink($temp_file);
  }
  return $branches;
}

/**
 * Parse the branch list output from Git.
 */
function _versioncontrol_git_log_parse_branches(&$logs) {
  // If the log was retrieved by taking the return value of exec(), we've
  // got an array and navigate it via next(). If we stored the log in a
  // temporary file, $logs is a file handle that we need to fgets() instead.
  $next = is_array($logs) ? 'next' : 'fgets';

  $branches = array();
  while (($line = $next($logs)) !== FALSE) {
    if (preg_match('/([^\s]+)$/', $line, $matches)) { // Remove whitespace and ensure that the active module symbol '*' doesn't get included.
      $branches[] = $matches[1];
    }
  }
  return $branches;
}

/**
 * Get tags from Git using 'tag -l' command.
 */
function _versioncontrol_git_log_get_tags() {
  $logs = _versioncontrol_git_log_exec('git tag -l', $temp_file); // Query tags.
  $tags = _versioncontrol_git_log_parse_tags($logs); // Parse output.
  if (variable_get('versioncontrol_git_log_use_file', 1)) { // Close file.
    fclose($logs);
    unlink($temp_file);
  }
  return $tags;
}

/**
 * Parse the tag list output from Git.
 */
function _versioncontrol_git_log_parse_tags(&$logs) {
  // If the log was retrieved by taking the return value of exec(), we've
  // got an array and navigate it via next(). If we stored the log in a
  // temporary file, $logs is a file handle that we need to fgets() instead.
  $next = is_array($logs) ? 'next' : 'fgets';

  $tags = array();
  while (($line = $next($logs)) !== FALSE) { // TODO Possible parse message associated with tag.
    if (preg_match('/([^\s]+)$/', $line, $matches)) { // Remove whitespace.
      $tag = new StdClass();
      $tag->name = $matches[1];

      // Query most recent commit in tag.
      $temp_file = '';
      $logs2 = _versioncontrol_git_log_exec('git log '. $tag->name .' -n 1', $temp_file);

      if (preg_match('/^commit (.+)$/', $next($logs2), $matches)) {
        $tag->revision = $matches[1];
      }

      if (variable_get('versioncontrol_git_log_use_file', 1)) { // Close file.
        fclose($logs2);
        unlink($temp_file);
      }

      $tags[] = $tag;
    }
  }
  return $tags;
}

/**
 * Update the database by processing and inserting the previously retrieved tags.
 *
 * @param array $repository The repository array, as given by the Version Control API.
 * @param array $tags List of tags to process.
 */
function _versioncontrol_git_log_process_tags($repository, $tags) {
  $tag_actions = array();

  foreach ($tags as $tag) {
    // Don't insert the same tag twice.
    $count = db_result(db_query(
      "SELECT COUNT(*)
       FROM {versioncontrol_git_tag_operations} gtag_op
        INNER JOIN {versioncontrol_operations} op
         ON gtag_op.vc_op_id = op.vc_op_id
        INNER JOIN {versioncontrol_tag_operations} tag_op
         ON gtag_op.vc_op_id = tag_op.vc_op_id
       WHERE op.repo_id = %d AND op.type = %d
        AND gtag_op.revision = '%s'",
      $repository['repo_id'], VERSIONCONTROL_OPERATION_TAG,
      $tag->revision
    ));
    if ($count > 0) {
      continue;
    }

    $tag_action = array(
      'tag_name' => $tag->name,
      'action' => VERSIONCONTROL_ACTION_ADDED,
      'date' => time(), // TODO Find out if date can be retrived.
      'username' => 'anonymous', // TODO Find out if username can be retrived.
      'repo_id' => $repository['repo_id'],
      'message' => '', // TODO Find out if message can be retrived.
      'git_specific' => array(
        'revision' => $tag->revision,
      )
    );
    $tagged_items = array();

    $tag_actions[$tag_action['date']][] = array($tag_action, $tagged_items);
  }

  ksort($tag_actions);
  foreach ($tag_actions as $date => $date_tag_actions) {
    foreach ($date_tag_actions as $tag_actions_info) {
      $tag_operation = array(
        'type' => VERSIONCONTROL_OPERATION_TAG,
        'repository' => $repository,
        'date' => $date,
        'username' => $tag_actions_info[0]['username'],
        'message' => $tag_actions_info[0]['message'],
        'revision' => $tag_actions_info[0]['git_specific']['revision'],
        'labels' => array(),
      );
      versioncontrol_insert_operation($tag_operation, $tag_actions_info);

      //versioncontrol_insert_tag_operation($tag_actions_info[0], $tag_actions_info[1]);
    }
  }
}

/**
 * Get all commits from Git using 'git log' command.
 */
function _versioncontrol_git_log_get_commits($repository, &$file_revisions, $branch, $range) {
  $command = "git log $range --numstat --summary --pretty=medium";
  $logs = _versioncontrol_git_log_exec($command, $temp_file);
  _versioncontrol_git_log_parse($repository, $logs, $file_revisions, $branch); // Parse the info from the raw output.

  if (variable_get('versioncontrol_git_log_use_file', 1)) {
    fclose($logs);
    unlink($temp_file);
  }

  watchdog('special', $command);
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
 * @param string $branch The curent branch.
 */
function _versioncontrol_git_log_parse($repository, &$logs, &$file_revisions, $branch) {
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
    $revision = $matches[1];

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
        $message .= trim($line) ."\n";
      }
      else {
        break;
      }
    }

    // Read file line revisions.
    $revisions = array();
    $duplicates = array();
    while (($line = $next($logs)) !== FALSE) {
      if (is_numeric($line[0]) && preg_match('/^(\S+)'."\t".'(\S+)'."\t".'(.+)$/', $line, $matches)) { // Begins with num lines added and matches expression.
        if (($file_revision = _versioncontrol_git_log_get_duplicate_revision($file_revisions, $revision)) !== NULL) {
          $file_revision->branches[] = $branch; // Add branch.
          $revisions[] = FALSE;
          $duplicates[] = count($revisions) - 1;
        }
        else {
          $file_revision = new StdClass();
          $file_revision->revision = $revision;
          $file_revision->username = $author;
          $file_revision->date = strtotime($date);
          $file_revision->message = $message;
          $file_revision->lines_added = $matches[1];
          $file_revision->lines_removed = $matches[2];
          $file_revision->path = '/'. $matches[3];
          $file_revision->action = VERSIONCONTROL_ACTION_MODIFIED;
          $file_revision->branches = array($branch);
          $revisions[] = $file_revision;
        }
      }
      else {
        break;
      }
    }

    // Read file actions.
    $i = 0;
    do {
      if (preg_match('/^ (\S+) (\S+) (\S+) (.+)$/', $line, $matches)) { // Ensure that same file, they should be in same order.
        if ($revisions[$i] !== FALSE) {
          $revisions[$i]->action = ($matches[1] == 'create' ? VERSIONCONTROL_ACTION_ADDED : VERSIONCONTROL_ACTION_DELETED);
        }
        $i++;
      }
      else {
        break;
      }
    }
    while (($line = $next($logs)) !== FALSE);

    // Remove duplicate place holders.
    foreach ($duplicates as $duplicate) {
      unset($revisions[$duplicate]);
    }

    $file_revisions = array_merge($file_revisions, $revisions);
  } // Loop to the next revision.
}

/**
 * Get an already existing revision from the $file_revisions array with
 * the specified revision hash.
 */
function _versioncontrol_git_log_get_duplicate_revision(&$file_revisions, $revision) {
  foreach ($file_revisions as $file_revision) {
    if ($file_revision->revision == $revision) {
      return $file_revision;
    }
  }
  return NULL;
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
    $vc_op_id = db_result(db_query(
      "SELECT op.vc_op_id
       FROM {versioncontrol_git_item_revisions} ir
        INNER JOIN {versioncontrol_operations} op
         ON ir.vc_op_id = op.vc_op_id
        INNER JOIN {versioncontrol_commits} c
         ON ir.vc_op_id = c.vc_op_id
       WHERE op.repo_id = %d AND op.type = %d
        AND ir.path = '%s' AND c.revision = '%s'",
      $repository['repo_id'], VERSIONCONTROL_OPERATION_COMMIT,
      $file_revision->path, $file_revision->revision
    ));
    if ($vc_op_id) {
      // Check to make it has been saved to all branches.
      foreach ($file_revision->branches as $branch) {
        $branch_id = versioncontrol_get_branch_id($branch, $repository['repo_id']);
        $count = db_result(db_query("SELECT COUNT(*)
          FROM {versioncontrol_git_commit_branches}
          WHERE vc_op_id = %d AND branch_id = %d",
          $vc_op_id, $branch_id));

        if ($count == 0) { // Add to branch.
           db_query("INSERT INTO {versioncontrol_git_commit_branches} (vc_op_id, branch_id) VALUES (%d, %d)",
             $vc_op_id, $branch_id);
        }
      }
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
        'revision' => $file_revision->revision,
      ),
      'source items' => array(
        array(
          'type' => VERSIONCONTROL_ITEM_FILE,
          'path' => $file_revision->path,
          'revision' => NULL, // Will be updated later.
        ),
      ),
      'git_specific' => array(
        'file_revision' => $file_revision, // Temporary.
        'lines_added' => $file_revision->lines_added,
        'lines_removed' => $file_revision->lines_removed,
        'revision' => $file_revision->revision,
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

    $commit_actions[$file_revision->revision][$file_revision->path] = $commit_action;
  }

  $commits = array();
  foreach ($commit_actions as $revision => $commit_actions) {
    _versioncontrol_git_log_construct_commit($repository, $commit_actions, $commits);
  }

  // Ok, we've got all commits gathered and in a nice array with
  // the commit date as key. So the only thing that's left is to sort them
  // and then send each commit to the API function for inserting into the db.
  ksort($commits);
  foreach ($commits as $date => $date_commits) {
    foreach ($date_commits as $commit_info) {
      // Record revision and source revision to database.
      foreach ($commit_info->commit_actions as $path => $commit_action) {
        // Update source revision.
        $commit_info->commit_actions[$path]['source items'][0]['revision'] = versioncontrol_git_get_current_revision($repository['repo_id'], $commit_action['source items'][0]['path']);
      }

      $commit_operation = array(
        'type' => VERSIONCONTROL_OPERATION_COMMIT,
        'repository' => $repository,
        'date' => $date,
        'username' => $commit_info->commit['username'],
        'message' => $commit_info->commit['message'],
        'revision' => $commit_info->commit['revision'],
        'labels' => array(),
      );
      versioncontrol_insert_operation($commit_operation, $commit_info->commit_actions);
    }
  }
}

/**
 * Use the additional file revision information that has been stored
 * in each commit action array in order to assemble the associated commit.
 * That commit information is then stored as a list item in the given
 * $commits array as an object with 'commit' and 'commit_actions' properties.
 */
function _versioncontrol_git_log_construct_commit($repository, $commit_actions, &$commits) {
  $date = 0;

  // Remove extra commit properties.
  foreach ($commit_actions as $path => $commit_action) {
    $file_revision = $commit_action['git_specific']['file_revision'];
    $revision = $commit_action['git_specific']['revision'];
    unset($commit_actions[$path]['git_specific']['file_revision']);
  }

  if ($file_revision->date > $date) {
    $date = $file_revision->date;
  }

  // Get the branch id, and insert the branch into the database
  // if it doesn't exist yet.
  $branch_ids = array();
  foreach ($file_revision->branches as $branch) {
    $branch_ids[] = versioncontrol_git_get_label_id($repository['repo_id'], $branch, VERSIONCONTROL_OPERATION_BRANCH);
  }

  // Yay, we have all commit actions and all information. Ready to go!
  $commit = array(
    'repo_id' => $repository['repo_id'],
    'date' => $date,
    'username' => $file_revision->username,
    'message' => $file_revision->message,
    'revision' => $revision,
    'git_specific' => array(
      'branch_ids' => $branch_ids,
    ),
  );

  $commit_info = new StdClass();
  $commit_info->commit = $commit;
  $commit_info->commit_actions = $commit_actions;
  $commits[$date][] = $commit_info;
}

/**
 * FIXME: api for retrieve branch id?
 * mimic some of version_control_ensure_branch()
 */
function versioncontrol_git_get_label_id($repo_id, $branch, $type) {
  $result = db_query('SELECT label_id FROM {versioncontrol_labels} WHERE repo_id = \'%s\' AND name = \'%s\' AND type = %s', $repository['repo_id'], $branch, VERSIONCONTROL_OPERATION_BRANCH);
  $branch_id = db_fetch_array($result);
  return $branch_id;
}
