<?php
// $Id: versioncontrol_git.log.inc,v 1.28 2009/04/11 14:38:45 corni Exp $
/**
 * @file
 * Git backend for Version Control API - Provides Git commit information and
 * account management as a pluggable backend.
 *
 * Copyright 2008 by Jimmy Berry ("boombatower", http://drupal.org/user/214218)
 * Copyright 2009 by Cornelius Riemenschneider ("CorniI", http://drupal.org/user/136353)
 */

/**
 * Actually update the repository by fetching commits and other stuff
 * directly from the repository, invoking the git executable.
 * @param $repository
 * @return
 *   TRUE if the logs were updated, or FALSE if fetching and updating the logs
 *   failed for whatever reason.
 */
function _versioncontrol_git_log_update_repository(&$repository) {
  $root = escapeshellcmd($repository['root']);
  $chdir_ok = @chdir($root); // Set working directory to root.
  if ($chdir_ok === FALSE) {
    return FALSE;
  }
  if ($repository['git_specific']['locked'] == TRUE) {
    drupal_set_message(t('This repository is locked, there is already a fetch in progress. If this is not the case, press the clear lock button.'), 'error');
    return FALSE;
  }
  db_query('UPDATE {versioncontrol_git_repositories}
            SET locked = 1 WHERE repo_id = %d', $repository['repo_id']);

  // Get the list of current branches from Git.
  $branch_list = _versioncontrol_git_log_get_branches();
  $add_branch_label = array(
        'name' => '', //filled later
        'type' => VERSIONCONTROL_OPERATION_BRANCH,
        'action' => VERSIONCONTROL_ACTION_MODIFIED
  );
  $branches = array();
  foreach ($branch_list as $branch_name) {
    $add_branch_label['name'] = $branch_name;
    $label_ret = versioncontrol_ensure_label($repository, $add_branch_label);
    $branches[$branch_name] = $label_ret;
  }
  //jpetso and me (corni) came to the conclusion that we will not delete branches.
  // Record new commits.
  $constraints = array(
    'vcs' => array('git'),
    'repo_ids' => array($repository['repo_id']),
    'types' => array(VERSIONCONTROL_OPERATION_COMMIT),
    'branches' => array() // used in the loop
  );
  $branches_per_commit = array();
  $existing_revs = array();
  // We cache the array in a temporary file to avoid the huge query when someone checks for new commits.
  $cached = _versioncontrol_git_get_cache($repository, $existing_revs);
  // Get the list of current branches from Git.
  // Generate the range per branch with which git shall be called.
  foreach ($branches as $branch_name => $label) {
    if (!$cached) {
      // We get all commits we have in this branch to not process them later.
      $constraints['branches'] = array($branch_name);
      $latest_commit_date = 0;
      $commit_op = versioncontrol_get_operations($constraints);
      $latest_commit = FALSE;
      foreach ($commit_op as $vc_op_id => $c_op) {
        if ($latest_commit_date < $c_op['date']) {
          $latest_commit = $c_op['revision'];
          $latest_commit_date = $c_op['date'];
          $existing_revs[$branch_name][$latest_commit] = TRUE;
        }
      }
    }
    // No way to free the damned mysql result!!
    unset($commit_op);

    $commits_in_branch = _versioncontrol_git_log_get_commits_in_branch($repository, escapeshellarg($branch_name));
    foreach ($commits_in_branch as $i => $commit) {
      if (!isset($existing_revs[$branch_name][$commit])) {
        if (!isset($branches_per_commit[$commit]) || !is_array($branches_per_commit[$commit])) {
          $branches_per_commit[$commit] = array($label);
        }
        else {
          $branches_per_commit[$commit][] = $label;
        }
      }
    }
  }
  // This uses an extra loop on purpose!
  // Process all commits on a per-branch base.
  foreach ($branches_per_commit as $revision => $branch) {
    // Update commits from Git.
    _versioncontrol_git_process_commits($repository, $revision, $branches_per_commit, $existing_revs);
  }
  // Check tags.
  $tags = _versioncontrol_git_log_get_tags(); //Now we have the current list of tags as array of strings.
  $constraints = array(
    'vcs' => array('git'),
    'repo_ids' => array($repository['repo_id']),
    'types' => array(VERSIONCONTROL_OPERATION_TAG)
  );
  $existing_tag_ops = versioncontrol_get_operations($constraints);
  $existing_tags = array();
  foreach ($existing_tag_ops as $tag_op) {
    if (!in_array($tag_op['labels'][0]['name'], $existing_tags)) {
      $existing_tags[] = $tag_op['labels'][0]['name'];
    }
  }
  // Deleting tags is *not* supported. Read the manual if you want to know why...
  // Check for new tags.
  $new_tags = array_diff($tags, $existing_tags);
  if (!empty($new_tags)) {
    _versioncontrol_git_process_tags($repository, $new_tags);
  }

  // Update repository updated field. Displayed on administration interface for documentation purposes.
  $repository['git_specific']['updated'] = time();
  db_query('UPDATE {versioncontrol_git_repositories}
            SET updated = %d, locked = 0 WHERE repo_id = %d',
            $repository['git_specific']['updated'], $repository['repo_id']);
  _versioncontrol_git_write_cache($repository, $existing_revs);
  return TRUE;
}

/**
 * Writes our existing_revs array to a file.
 */
function _versioncontrol_git_write_cache(&$repository, &$existing_revs) {
  $file = "<?php\n" .'$_versioncontrol_git_cache_var = ';
  $file .= var_export($existing_revs, TRUE);
  $file .= ';';
  $path = '';
  if (!file_exists($repository['git_specific']['cache_path'])) {
    $key = sha1(mt_rand() + mt_rand());
    $path = file_directory_temp();
    file_check_directory($path, 3);
    $path .= '/versioncontrol_git_cache_'. $key .'.php';
    $repository['git_specific']['cache_path'] = $path;
    db_query('UPDATE {versioncontrol_git_repositories}
            SET cache_path = \'%s\' WHERE repo_id = %d',
    serialize($path), $repository['repo_id']);
  }
  $path = $repository['git_specific']['cache_path'];
  // Lock the file, as we may else have concurrent writes to the file, which would be *very* bad.
  file_put_contents($path, $file, LOCK_EX);
}

/**
 * This function loads the array $existing_revs with the cache file from $repository.
 */
function _versioncontrol_git_get_cache($repository, &$existing_revs) {
  $path = $repository['git_specific']['cache_path'];
  if (file_exists($path)) {
    include $path;
    $existing_revs = $_versioncontrol_git_cache_var;
    unset($_versioncontrol_git_cache_var);
  }
  else {
    $existing_revs = array();
    return FALSE;
  }
  return TRUE;
}

/**
 * Execute a Git command using the root context and the command to be executed.
 * @param string $command Command to execute.
 * @return mixed Logged output from the command in either array of file pointer form.
 */
function _versioncontrol_git_log_exec($command) {
  $logs = array();
  exec($command, $logs);
  array_unshift($logs, '');
  reset($logs); // Reset the array pointer, so that we can use next().
  return $logs;
}

/**
 * Get branches from Git using 'branch -l' command.
 * @return array List of branches.
 */
function _versioncontrol_git_log_get_branches() {
  $logs = _versioncontrol_git_log_exec('git show-ref --heads'); // Query branches.
  $branches = _versioncontrol_git_log_parse_branches($logs); // Parse output.
  return $branches;
}

/**
 * Parse the branch list output from Git.
 */
function _versioncontrol_git_log_parse_branches(&$logs) {
  $branches = array();
  while (($line = next($logs)) !== FALSE) {
    $branches[] = substr(trim($line), 52);
  }
  return $branches;
}

/**
 * Get tags from Git using 'tag -l' command.
 */
function _versioncontrol_git_log_get_tags() {
  //TODO: incorporate a --dereference and parse better, saves one git call for tags
  $logs = _versioncontrol_git_log_exec('git show-ref --tags'); // Query tags.
  $tags = _versioncontrol_git_log_parse_tags($logs); // Parse output.
  return $tags;
}

/**
 * Parse the tag list output from Git.
 */
function _versioncontrol_git_log_parse_tags(&$logs) {
  $tags = array();
  while (($line = next($logs)) !== FALSE) {
    $tags[] = $branches[] = substr(trim($line), 51);
  }
  return $tags;
}

/**
 * Parses output of git show $tag_name provided by _versioncontrol_git_get_tag_operation() to retrieve an $operation for inserting a tag.
 * @param $repository
 * @param sring $tag_name The name of the parsed tag
 * @param $logs The output of git show
 * @return array An $operation array which contains the info for the tag.
 */
function _versioncontrol_git_log_parse_tag_info($repository, &$logs, $tag_commits) {
  $line = next($logs); // Get op type
  if ($line === FALSE) {
    return FALSE;
  }
  if ($line == 'commit') {
    //let's get the author and the date from the tagged commit, better than nothing.
    $tagged_commit = next($logs); // Get the tagged commit
    $tag_name = next($logs); // Get the name of the tag
    next($logs); // Skip these two lines
    next($logs);
    // Get the tag/commit message
    $message = '';
    $i = 0;
    while (($line = next($logs)) !== FALSE) {
      if ($line == 'ENDOFGITTAGOUTPUTMESAGEHERE') {
        break;
      }
      if ($i == 1) {
        $message .= "\n";
      }
      $message .= $line ."\n";
      $i++;
    }
    $constraints = array(
      'vcs' => array('git'),
      'repo_ids' => array($repository['repo_id']),
      'types' => array(VERSIONCONTROL_OPERATION_COMMIT),
      'revisions' => array($tagged_commit)
    );
    $op = versioncontrol_get_operations($constraints);
    $op = array_pop($op);
    return array(
      'type' => VERSIONCONTROL_OPERATION_TAG,
      'repository' => $repository,
      'date' => $op['date']+1, // We want to be displayed *after* the tagged commit.
      'username' => $op['username'],
      'message' => $message,
      'revision' => $tagged_commit,
      'labels' => array(
        0 => array(
          'name' => $tag_name,
          'type' => VERSIONCONTROL_OPERATION_TAG,
          'action' => VERSIONCONTROL_ACTION_ADDED
        )
      )
    );
  }
  $line = next($logs); // Skip op sha1
  $tag_name = next($logs); // Get tag name
  $tagger = next($logs); // Get tagger
  $date = strtotime(next($logs)); // Get date
  // Get the tag message
  $message = '';
  $i = 0;
  while (($line = next($logs)) !== FALSE) {
    if ($line == 'ENDOFGITTAGOUTPUTMESAGEHERE') {
      break;
    }
    if ($i == 1) {
      $message .= "\n";
    }
    $message .= $line ."\n";
    $i++;
  }
  $tagged_commit = $tag_commits[$tag_name];
  // By now, we're done with the parsing, construct the op array
  return array(
    'type' => VERSIONCONTROL_OPERATION_TAG,
    'repository' => $repository,
    'date' => $date,
    'username' => $tagger,
    'message' => $message,
    'revision' => $tagged_commit,
    'labels' => array(
      0 => array(
        'name' => $tag_name,
        'type' => VERSIONCONTROL_OPERATION_TAG,
        'action' => VERSIONCONTROL_ACTION_ADDED
      )
    )
  );
}

/**
 * Invokes 'git-show tag' to get information about a tag.
 * It's output is later parsed by _versioncontrol_git_log_parse_tag_info().
 * @param $repository
 * @param string $tag The name of the tag.
 * @return An $operation array which contains the info for the tag.
 */
function _versioncontrol_git_get_tag_operations($repository, $tags) {
  $tag_ops = array();
  $tag_string = '';
  if (empty($tags)) {
    return array();
  }
  foreach ($tags as $tag) {
    $tag_string .= escapeshellarg("refs/tags/$tag") .' ';
  }
  $format = "%(objecttype)\n%(objectname)\n%(refname:short)\n%(taggername) %(taggeremail)\n%(taggerdate)\n%(contents)\nENDOFGITTAGOUTPUTMESAGEHERE";
  $exec = "git for-each-ref --format=\"$format\" $tag_string";
  $logs_tag_msg = _versioncontrol_git_log_exec($exec);
  $exec = "git show-ref -d $tag_string";
  $logs_tag_commits = _versioncontrol_git_log_exec($exec);
  $tag_commits = array();
  foreach ($logs_tag_commits as $line) {
    if (substr($line, -3, 3) == '^{}') {
      $commit = substr($line, 0, 40);
      $tag = substr($line, 41);
      $tag = substr(substr($line, 41, strlen($tag) -3), 10);
      $tag_commits[$tag] = $commit;
    }
  }
  do {
    $ret = _versioncontrol_git_log_parse_tag_info($repository, $logs_tag_msg, $tag_commits);
    if ($ret !== FALSE) {
      $tag_ops[] = $ret;
    }
  }while ($ret !== FALSE);
  return $tag_ops;
}

/**
 * Does all the processing for all new tags.
 * @param $repository
 * @param array $new_tags An array of strings for all new tags which shall be processed
 */
function _versioncontrol_git_process_tags($repository, $new_tags) {
  $tag_ops = _versioncontrol_git_get_tag_operations($repository, $new_tags);
  foreach ($tag_ops as $tag_op) {
    $op_items = array();
    versioncontrol_insert_operation($tag_op, $op_items);
    $constraints = array(
      'vcs' => array('git'),
      'repo_ids' => array($repository['repo_id']),
      'types' => array(VERSIONCONTROL_OPERATION_COMMIT),
      'revisions' => array($tag_op['revision'])
    );
    $tag_commits = versioncontrol_get_operations($constraints);
    foreach ($tag_commits as $vc_op_id => $tag_commit_op) {
      $tag_commit_op['labels'][] = array(
        'name' => $tag_op['labels'][0]['name'],
        'action' => VERSIONCONTROL_ACTION_MODIFIED,
        'type' => VERSIONCONTROL_OPERATION_TAG
      );
      versioncontrol_update_operation_labels($tag_commit_op, $tag_commit_op['labels']);
    }
  }
}

/**
 * Get all commits from Git using 'git log' command.
 * @param $repository
 * @param string $range the computed range for the branch we check
 * @param array $branches_per_commit An array of all commits we will encounter with a list of branches they are in.
 */
function _versioncontrol_git_process_commits($repository, $revision, &$branches_per_commit, &$existing_revs) {
  $rev_shell = escapeshellarg($revision);
  $command = "git log $rev_shell --numstat --summary --pretty=format:\"%H%n%P%n%aN <%ae>%n%ct%n%s%n%b%nENDOFOUTPUTGITMESSAGEHERE\" -n 1 --";
  $logs = _versioncontrol_git_log_exec($command);
  _versioncontrol_git_log_parse_commits($repository, $logs, $branches_per_commit, $existing_revs); // Parse the info from the raw output.
}

/**
 * This function returns all commits in the given range.
 * It is used to get all new commits in a branch, which is specified by @p $range
 * @param $repository
 * @param string $range The range of the commits to retrieve
 * @return array An array of strings with all commit id's in it
 */
function _versioncontrol_git_log_get_commits_in_branch($repository, $range) {
  $logs = _versioncontrol_git_log_exec("git rev-list $range --reverse --"); // Query tags.
  $commits = array();
  while (($line = next($logs)) !== FALSE) {
    $commits[] = trim($line);
  }
  return $commits;
}

/**
 * A helper function to get the source_items.
 * @param $repository
 * @param $revision The revision of the parent item.
 * @param $filename The filename of the parent item.
 * @return array An $item array ready to use for $operation['source_items']
 */
function _versioncontrol_git_get_source_item_helper($repository, $revision, $filename, $branches) {
  $branch_names = array();
  foreach ($branches as $branch) {
    $branch_names[] = $branch['name'];
  }
  $constraints = array(
    'vcs' => array('git'),
    'repo_ids' => array($repository['repo_id']),
    'types' => array(VERSIONCONTROL_OPERATION_COMMIT),
    'paths' => array($filename),
    'branches' => $branch_names
  );
  $commit_op = versioncontrol_get_operations($constraints);
  ksort($commit_op);
  $commit_op = array_pop($commit_op);
  $op_items = versioncontrol_get_operation_items($commit_op);
  $type = $op_items[$filename]['type'] ? $op_items[$filename]['type'] : VERSIONCONTROL_ITEM_FILE;
  // ['action'] not needed for source items :)
  return array(
    'path' => $filename,
    'type' => $type,
    'revision' => $op_items[$filename]['revision'],
  );
}

/**
 * A function to a source_item for a specific file.
 * @param $repository as we get it from the API
 * @param string $filename the revision of the current item we shall get it's source from
 * @return array $source_items array for use in an $operation.
 */
function _versioncontrol_git_get_source_item($repository, $filename, $parents, $branches, $commit_rev) {
  $ret = array();
  if (count($parents) == 1) {
      $filenameg = substr($filename, 1);
      $filenameg = escapeshellarg($filenameg);
      $commit_rev = escapeshellarg($commit_rev);
      $exec = "git rev-list -n 1 ". $commit_rev ."^ -- $filenameg";
      $logs = _versioncontrol_git_log_exec($exec); // Query tags.
      $revision = next($logs);
      $ret = array(
        array(
          'path' => $filename,
          'type' => VERSIONCONTROL_ITEM_FILE,
          'revision' => $revision
        )
      );
  }
  else {
    foreach ($parents as $rev) {
      $ret[] = _versioncontrol_git_get_source_item_helper($repository, $rev, $filename, $branches);
    }
  }
  return $ret;
}

function _versioncontrol_git_insert_commit($repository, $date, $username, $message, $revision, $branches, $op_items) {
  $op = array(
    'type' => VERSIONCONTROL_OPERATION_COMMIT,
    'repository' => $repository,
    'date' => $date,
    'username' => $username,//filled later
    'message' => $message, //filled later
    'revision' => $revision, //filled later
    'labels' => $branches
  );
  versioncontrol_insert_operation($op, $op_items);
}

function _versioncontrol_git_parse_items($repository, &$logs, &$line, $revision, &$branches_per_commit, $parents) {
  $op_items = array();
  $read = FALSE;

  // Read file line revisions.
  do {
    if (preg_match('/^(\S+)'."\t".'(\S+)'."\t".'(.+)$/', $line, $matches)) { // Begins with num lines added and matches expression.
      $read = TRUE;
      $path = '/'. $matches[3];
      $op_items[$path] = array(
          'type' => VERSIONCONTROL_ITEM_FILE,
          'path' => $path,
          'source_items' => array(),//filled later
          'action' => $merge ? VERSIONCONTROL_ACTION_MERGED : VERSIONCONTROL_ACTION_MODIFIED,
          'revision' => $revision
      );
      if (is_numeric($matches[1] && is_numeric($matches[2]))) {
        $op_items[$path]['line_changes'] = array(
            'added' => $matches[1],
            'removed' => $matches[2]
        );
      }
    }
    else {
      break;
    }
  } while (($line = next($logs)) !== FALSE);
  // Read file actions.
  do {
    if (preg_match('/^ (\S+) (\S+) (\S+) (.+)$/', $line, $matches)) { // Ensure that same file, they should be in same order.
      $read = TRUE;
      // We also can get 'mode' here if someone changes the file permissions.
      if ($matches[1] == 'create') {
        $op_items['/'. $matches[4]]['action'] = VERSIONCONTROL_ACTION_ADDED;
      }
      else if ($matches[1] == 'delete') {
        $op_items['/'. $matches[4]]['action'] = VERSIONCONTROL_ACTION_DELETED;
      }
    }
    else {
      break;
    }
  }
  while (($line = next($logs)) !== FALSE);

  //This is an inconsistency in git log output...
  if ($read) {
    $line = next($logs);
  }
  foreach ($op_items as $path => $item) {
    if ($item['action'] != VERSIONCONTROL_ACTION_ADDED) {
      $op_items[$path]['source_items'] = _versioncontrol_git_get_source_item($repository, $path, $parents, $branches_per_commit[$revision], $revision);
    }
  }
  return $op_items;
}

function _versioncontrol_git_check_already_parsed_commits($repository, $revision, &$branches_per_commit, &$line, &$logs) {
  $constraints = array(
      'types' => array(VERSIONCONTROL_OPERATION_COMMIT),
      'revisions' => array($revision),
      'vcs' => array('git'),
      'repo_ids' => array($repository['repo_id'])
  );
  $same_rev_commit = versioncontrol_get_operations($constraints);
  $adjusted_commit = FALSE;
  foreach ($same_rev_commit as $vc_op_id => $rev_commit) {
    // We already have a commit with this revision recorded, so use a faster parser then.
    $adjusted_commit = TRUE;
    $labels = array();
    foreach ( $rev_commit['labels'] as $label) {
      if ($label['type'] == VERSIONCONTROL_OPERATION_TAG) {
        $labels[] = $label;
      }
    }
    $labels = array_merge($labels, $branches_per_commit[$revision]);
    versioncontrol_update_operation_labels($rev_commit, $labels);
    $line = next($logs); // Get $parents
    $line = next($logs); // Get Author
    $line = next($logs); // Get Date as Timestamp
    // Pretend message parsing
    while (($line = next($logs)) !== FALSE) {
      if (trim($line) == 'ENDOFOUTPUTGITMESSAGEHERE') {
        break;
      }
    }
    $line = next($logs);
    // Skip everything --summary or --numstat related output
    while (!(preg_match("/^([a-f0-9]{40})$/", trim($line))) && $line !== FALSE) {
      $line = next($logs);
    }
    //$branches_per_commit[$revision] = TRUE;
  }
  return $adjusted_commit;
}

/**
 * Parse the output of 'git log' and insert commits based on it's data.
 *
 * @param $repository
 *   The repository array, as given by the Version Control API.
 * @param $logs The output of 'git log' to parse
 * @param array $branches_per_commit An array which has all branches for all commits in it.
 * 	It is used to construct $operation['labels'].
 */
function _versioncontrol_git_log_parse_commits($repository, &$logs, &$branches_per_commit, &$existing_revs) {
  // If the log was retrieved by taking the return value of exec(), we've
  // got an array and navigate it via next(). If we stored the log in a
  // temporary file, $logs is a file handle that we need to fgets() instead.
  $root_path = $repository['root'];
  $line = next($logs); // Get Revision
  $merge = FALSE;
  // $line already points to the revision
  $revision = trim($line);
  foreach ($branches_per_commit[$revision] as $label) {
    $existing_revs[$label['name']][$revision] = TRUE;
  }

  $adjusted_commit = _versioncontrol_git_check_already_parsed_commits($repository,
    $revision, $branches_per_commit, $line, $logs);
  if ($adjusted_commit) {
    return;
  }

  $line = next($logs); // Get $parents
  $parents = explode(" ", trim($line));
  if ($parents[0] == '') {
    $parents = array();
  }
  if (isset($parents[1])) {
    $merge = TRUE;
  }
  $line = next($logs); // Get Author
  $username = trim($line);
  $line = next($logs); // Get Date as Timestamp
  $date = trim($line);
  // Get revision message.
  $message = '';
  $i = 0;
  while (($line = next($logs)) !== FALSE) {
    $line = trim($line);
    if ($line == 'ENDOFOUTPUTGITMESSAGEHERE') {
      if (substr($message, -2) === "\n\n") {
        $message = substr($message, 0, strlen($message) - 1);
      }
      break;
    }
    if ($i == 1) {
      $message .= "\n";
    }
    $message .= $line ."\n";
    $i++;
  }
  $line = next($logs); // Points to either the next entry or the first items modified or to the file actions
  // Get the items
  $op_items = _versioncontrol_git_parse_items($repository, $logs, $line, $revision,
  $branches_per_commit, $parents);

  _versioncontrol_git_insert_commit($repository, $date, $username,
  $message, $revision, $branches_per_commit[$revision], $op_items);
}
