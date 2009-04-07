<?php
// $Id: versioncontrol_git.log.inc,v 1.27.2.1 2009/04/04 13:54:58 marvil07 Exp $
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
  //TODO: fix the log file or remove it.
  variable_set('versioncontrol_git_log_use_file', 0);
  
  $root = escapeshellcmd($repository['root']);
  $chdir_ok = chdir($root); // Set working directory to root.
  if ($chdir_ok === FALSE) {
    return FALSE;
  }

  // Get the list of current branches from Git.
  $branch_list = _versioncontrol_git_log_get_branches();
  // Get the existing branches.
  /*$existing_branches = versioncontrol_get_label_list($repository, VERSIONCONTROL_OPERATION_BRANCH);
  // Check for new branches.
  $new_branches = array_diff($branches, $existing_branches);*/
  $add_branch_label =array(
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
  $options = array(
    'query_type' => 'range',
    'count' => 1,
    'from' => 0
  );
  $ranges = array();
  $branches_per_commit = array();
  // Get the list of current branches from Git.
  // Generate the range per branch with which git shall be called.
  foreach ($branches as $branch_name => $label) {
    $constraints['branches'] = array($branch_name);
    $commit_op = versioncontrol_get_operations($constraints, $options);
    $latest_commit = FALSE;
    if (!empty($commit_op)) {
      foreach ($commit_op as $vc_op_id => $c_op) {
        $latest_commit = $c_op['revision'];
      }
    }
    // Generate either '[branch]' or '[hash]..[branch]' as range specification for git log.
    $range = $latest_commit ? ($latest_commit .'..') : '';
    $range .= $branch_name;
    $ranges[$branch_name] = $range;
    $commits_in_branch = _versioncontrol_git_log_get_commits_in_branch($repository, $range);
    foreach ($commits_in_branch as $commit) {
      if (!isset($branches_per_commit[$commit]) || !is_array($branches_per_commit[$commit])) {
        $branches_per_commit[$commit] = array($label);
      }
      else
      {
        $branches_per_commit[$commit][] = $label;
      }
    }
  }
  // This uses an extra loop on purpose!
  // Process all commits on a per-branch base.
  foreach ($branches as $branch_name => $label) {
    // Update commits from Git.
    _versioncontrol_git_process_commits($repository, $ranges[$branch_name], $branches_per_commit);
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
  foreach($existing_tag_ops as $tag_op) {
    if(!in_array($tag_op['labels'][0]['name'], $existing_tags)) {
      $existing_tags[] = $tag_op['labels'][0]['name'];
    }
  }
  // Deleting tags is *not* supported. Read the manual if you want to know why...
  // Check for new tags.
  $new_tags = array_diff($tags, $existing_tags);
  _versioncontrol_git_process_tags($repository, $new_tags);

  // Update repository updated field. Displayed on administration interface for documentation purposes.
  $repository['git_specific']['updated'] = time();
  db_query('UPDATE {versioncontrol_git_repositories}
            SET updated = %d WHERE repo_id = %d',
            $repository['git_specific']['updated'], $repository['repo_id']);
  return TRUE;
}
/**
 * Execute a Git command using the root context and the command to be executed.
 * 
 * @param string $command Command to execute.
 * @param reference $temp_file Reference to temporaray file.
 * @return mixed Logged output from the command in either array of file pointer form.
 */
function _versioncontrol_git_log_exec($command, &$temp_file) {
  watchdog('special', $command);
  /*if (variable_get('versioncontrol_git_log_use_file', 1)) {
    $temp_dir = variable_get('file_directory_temp', (PHP_OS == 'WINNT' ? 'c:\\windows\\temp' : '/tmp'));
    $temp_file = $temp_dir .'/git-'. rand();
    exec("$command > $temp_file");
    $logs = fopen($temp_file, 'r');
  }
  else*/ {
    $logs = array();
    exec($command, $logs);
    array_unshift($logs,'');
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
      $tags[] = $matches[1];
    }
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
function _versioncontrol_git_log_parse_tag_info($repository, $tag_name, &$logs) {
  $matches = array();
  $next = is_array($logs) ? 'next' : 'fgets';
  $line = next($logs); // Check the first line for an un-annotated tag
  if (trim($line) == 'ENDOFGITTAGOUTPUTMESAGEHERE') {
    //let's get the author and the date from the tagged commit, better than nothing.
    $line = next($logs); // Get the tagged commit
    $tagged_commit = trim($line);
    $constraints = array(
      'vcs' => array('git'),
      'repo_ids' => array($repository['repo_id']),
      'types' => array(VERSIONCONTROL_OPERATION_COMMIT),
      'revisions' => array($tagged_commit)
    );
    $op = versioncontrol_get_operations($constraints);
    $op = array_pop($op);
    return array (
      'type' => VERSIONCONTROL_OPERATION_TAG,
      'repository' => $repository,
      'date' => $op['date']+1, // We want to be displayed *after* the tagged commit.
      'username' => $op['username'],
      'message' => "Non-Annotated tag of '$tag_name'\nAttention, tagger and date are guessed, please use 'git tag -a' the next time you tag.",
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
  $line = next($logs); // Get the tagger
  preg_match('/^Tagger: (.+)$/', $line, $matches);
  $tagger = trim($matches[1]);
  $line = next($logs); // Get date
  preg_match('/^Date: (.+)$/', $line, $matches);
  $date = strtotime($matches[1]);
  $line = next($logs); // Empty line
  // Get the tag message
  $message = '';
  $i = 0;
  while (($line = $next($logs)) !== FALSE) {
    if (trim($line) == 'ENDOFGITTAGOUTPUTMESAGEHERE') {
      break;
    }
    if ($i == 1) {
      $message .= "\n";
    }
    $message .= trim($line) ."\n";
    $i++;
  }
  $line = next($logs); // Get the tagged commit
  $tagged_commit = trim($line);
  // By now, we're done with the parsing, construct the op array
  return array (
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
function _versioncontrol_git_get_tag_operation($repository, $tag) {
  $logs = _versioncontrol_git_log_exec("git show $tag --pretty=format:\"ENDOFGITTAGOUTPUTMESAGEHERE%n%H\" --", $temp_file); // Query tags.
  $tag_op = _versioncontrol_git_log_parse_tag_info($repository, $tag, $logs); // Parse output.
  if (variable_get('versioncontrol_git_log_use_file', 1)) { // Close file.
    fclose($logs);
    unlink($temp_file);
  }
  return $tag_op;
}

/**
 * Does all the processing for all new tags.
 * @param $repository
 * @param array $new_tags An array of strings for all new tags which shall be processed
 */
function _versioncontrol_git_process_tags($repository, $new_tags) {
  foreach ($new_tags as $tag) {
    $tag_op = _versioncontrol_git_get_tag_operation($repository, $tag);
    $op_items = array();
    versioncontrol_insert_operation($tag_op, $op_items);
    $constraints = array(
      'vcs' => array('git'),
      'repo_ids' => array($repository['repo_id']),
      'types' => array(VERSIONCONTROL_OPERATION_COMMIT),
      'revisions' => array($tag_op['revision'])
    );
    $tag_commit = versioncontrol_get_operations($constraints);
    foreach ($tag_commit as $vc_op_id => $tag_commit_op) {
      $tag_commit_items = versioncontrol_get_operation_items($tag_commit_op, TRUE);
      versioncontrol_delete_operation($tag_commit[$vc_op_id]);
      $tag_commit_op['labels'][] = array(
        'name' => $tag,
        'action' => VERSIONCONTROL_ACTION_MODIFIED,
        'type' => VERSIONCONTROL_OPERATION_TAG
      );
      versioncontrol_insert_operation($tag_commit_op, $tag_commit_items);
    }
  }
}


/**
 * Get all commits from Git using 'git log' command.
 * @param $repository
 * @param string $range the computed range for the branch we check
 * @param array $branches_per_commit An array of all commits we will encounter with a list of branches they are in.
 */
function _versioncontrol_git_process_commits($repository, $range, $branches_per_commit) {
  $command = "git log $range --numstat --summary --pretty=format:\"%H%n%P%n%aN <%ae>%n%at%n%s%n%bENDOFOUTPUTGITMESSAGEHERE\" --reverse --";
  $logs = _versioncontrol_git_log_exec($command, $temp_file);
  _versioncontrol_git_log_parse_commits($repository, $logs, $branches_per_commit); // Parse the info from the raw output.
  if (variable_get('versioncontrol_git_log_use_file', 1)) {
    fclose($logs);
    unlink($temp_file);
  }
}

/**
 * This function returns all commits in the given range.
 * It is used to get all new commits in a branch, which is specified by @p $range
 * @param $repository
 * @param string $range The range of the commits to retrieve
 * @return array An array of strings with all commit id's in it
 */
function _versioncontrol_git_log_get_commits_in_branch($repository, $range) {
  $logs = _versioncontrol_git_log_exec("git rev-list $range --", $temp_file); // Query tags.
  $next = is_array($logs) ? 'next' : 'fgets';
  $commits = array();
  while (($line = $next($logs)) !== FALSE) {
    $commits[] = trim($line);
  }
  if (variable_get('versioncontrol_git_log_use_file', 1)) { // Close file.
    fclose($logs);
    unlink($temp_file);
  }
  return $commits;
}

/**
 * 
 * @param $repository
 * @param $revision The revision of the parent item.
 * @param $filename The filename of the parent item.
 * @return array An $item array ready to use for $operation['source_items']
 */
function _versioncontrol_git_get_source_item_helper($repository, $revision, $filename) {
  $constraints = array(
    'vcs' => array('git'),
    'repo_ids' => array($repository['repo_id']),
    'types' => array(VERSIONCONTROL_OPERATION_COMMIT),
    'paths' => array($filename)
  );
  $options = array(
    'query_type' => 'range',
    'count' => 1,
    'from' => 0
  );
  $commit_op = versioncontrol_get_operations($constraints, $options);
  $commit_key = 0;
  foreach ($commit_op as $key => $op) {
    $commit_key = $key;
  }
  $op_items = versioncontrol_get_operation_items($commit_op[$commit_key]);
  $type = $op_items[$filename]['type'] ? $op_items[$filename]['type'] : VERSIONCONTROL_ITEM_FILE;
  $action = $op_items[$filename]['action'] ? $op_items[$filename]['action'] : VERSIONCONTROL_ACTION_ADDED;
  return array(
    'path' => $filename,
    'type' => $type,
    'revision' => $op_items[$filename]['revision'],
    'action' => $action
  );
}

/**
 * 
 * @param $repository as we get it from the API
 * @param string $filename the revision of the current item we shall get it's source from
 * @return array $source_items array for use in an $operation. 
 */
function _versioncontrol_git_get_source_item($repository, $filename, $parents) {
  $ret = array();
  foreach ($parents as $rev) {
    $ret[] = _versioncontrol_git_get_source_item_helper($repository, $rev, $filename);
  }
  return $ret;
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
function _versioncontrol_git_log_parse_commits($repository, &$logs, $branches_per_commit) {
  // If the log was retrieved by taking the return value of exec(), we've
  // got an array and navigate it via next(). If we stored the log in a
  // temporary file, $logs is a file handle that we need to fgets() instead.
  $next = is_array($logs) ? 'next' : 'fgets';
  $root_path = $repository['root'];
  $line = TRUE;
  $line = $next($logs); // Get Revision
  while ($line !== FALSE) {
    $merge = FALSE;
    $op = array(
      'type' => VERSIONCONTROL_OPERATION_COMMIT,
      'repository' => $repository,
      'date' => 0, //filled later
      'username' => '',//filled later
      'message' => '', //filled later
      'revision' => '', //filled later
      'labels' => array()
    );
    // $line already points to the revision
    $revision = trim($line);
    $constraints = array(
      'types' => array(VERSIONCONTROL_OPERATION_COMMIT),
      'revisions' => array($revision),
      'vcs' => array('git'),
      'repo_ids' => array($repository['repo_id'])
    );
    $same_rev_commit = versioncontrol_get_operations($constraints);
    $adjusted_commit = FALSE;
    foreach ($same_rev_commit as $vc_op_id => $rev_commit) {
      $adjusted_commit = TRUE;
      // We already have a commit with this revision recorded, so use a faster parser then.
      $rev_commit_items = versioncontrol_get_operation_items($rev_commit, TRUE);
      versioncontrol_delete_operation($rev_commit);
      // Use the updated labels array
      $rev_commit['labels'] = $branches_per_commit[$revision];
      versioncontrol_insert_operation($rev_commit, $rev_commit_items);
      // We'll deal with branches somewhere else in a more efficient way :)
      $line = $next($logs); // Get $parents
      $line = $next($logs); // Get Author
      $line = $next($logs); // Get Date as Timestamp
      // Pretend message parsing
      while (($line = $next($logs)) !== FALSE) {
        if (trim($line) == 'ENDOFOUTPUTGITMESSAGEHERE') {
          break;
        }
      }
      $line = $next($logs);
      // Skip everything --summary or --numstat related output
      while (!(preg_match("/^([a-f0-9]{40})$/", trim($line))) && $line !== FALSE) {
        $line = $next($logs);
      }
    }
    // We can't do a continue for the big while in the foreach loop.
    if ($adjusted_commit) {
      continue;
    }
    
    $op['revision'] = $revision;
    $line = $next($logs); // Get $parents
    $parents = explode(" ", trim($line));
    if ($parents[0] == '') {
      $parents = array();
    }
    if(isset($parents[1])) {
      $merge = TRUE;
    }
    $line = $next($logs); // Get Author
    $op['username'] = trim($line);
    $line = $next($logs); // Get Date as Timestamp
    $op['date'] = trim($line);
    // Get revision message.
    $message = '';
    $i = 0;
    while (($line = $next($logs)) !== FALSE) {
      if (trim($line) == 'ENDOFOUTPUTGITMESSAGEHERE') {
        break;
      }
      if ($i == 1) {
        $message .= "\n";
      }
      $message .= trim($line) ."\n";
      $i++;
    }
    $op['message'] = $message;
    $op_items = array();
    $read = FALSE;
    $line = $next($logs); // Points to either the next entry or the first items modified or to the file actions
    // Read file line revisions.
    do {
      if (preg_match('/^(\S+)'."\t".'(\S+)'."\t".'(.+)$/', $line, $matches)) { // Begins with num lines added and matches expression.
        $read = TRUE;
        $path = '/' . $matches[3];
        $op_items[$path] = array(
          'type' => VERSIONCONTROL_ITEM_FILE,
          'path' => $path,
          'source_items' => array(),//filled later
          'action' => $merge ? VERSIONCONTROL_ACTION_MERGED : VERSIONCONTROL_ACTION_MODIFIED,
          'revision' => $revision
        );
      }
      else {
        break;
      }
    } while (($line = $next($logs)) !== FALSE);
    // Read file actions.
    do {
      if (preg_match('/^ (\S+) (\S+) (\S+) (.+)$/', $line, $matches)) { // Ensure that same file, they should be in same order.
        $read = TRUE;
        $op_items['/' . $matches[4]]['action'] = ($matches[1] == 'create' ? VERSIONCONTROL_ACTION_ADDED : VERSIONCONTROL_ACTION_DELETED);
      }
      else {
        break;
      }
    }
    while (($line = $next($logs)) !== FALSE);
    
    //This is an inconsistency in got log output...
    if ($read) {
      $line = $next($logs);
    }

    foreach($op_items as $path => $item) {
      if($item['action'] != VERSIONCONTROL_ACTION_ADDED) {
        $op_items[$path]['source_items'] = _versioncontrol_git_get_source_item($repository, $path, $parents); 
      }
    }
    // Get the branches affected by the commit
    $op['labels'] = $branches_per_commit[$revision];
    versioncontrol_insert_operation($op, $op_items);
  } // Loop to the next revision.
}
