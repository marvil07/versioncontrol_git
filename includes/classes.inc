<?php

require_once drupal_get_path('module', 'versioncontrol') . '/includes/VersioncontrolBackend.php';

class VersioncontrolGitBackend extends VersioncontrolBackend {

  public function __construct() {
    $this->name = 'Git';
    $this->description = t('Git is a fast, scalable, distributed revision control system with an unusually rich command set that provides both high-level operations and full access to internals.');
    $this->capabilities = array(
        // Use the commit hash for to identify the commit instead of an individual
        // revision for each file.
        VERSIONCONTROL_CAPABILITY_ATOMIC_COMMITS
    );
    $this->classes = array(
      'repo' => 'VersioncontrolGitRepository',
      'account' => 'VersioncontrolGitAccount',
      'operation' => 'VersioncontrolGitOperation',
    );
  }

}

class VersioncontrolGitRepository extends VersioncontrolRepository {

  /**
   * Overwrite to get short sha-1's
   */
  public function formatRevisionIdentifier($revision, $format = 'full') {
    switch ($format) {
    case 'short':
      // Let's return only the first 7 characters of the revision identifier,
      // like git log --abbrev-commit does by default.
      return substr($revision, 0, 7);
    case 'full':
    default:
      return $revision;
    }
  }

}

class VersioncontrolGitAccount extends VersioncontrolAccount {

  /**
   * Overwrite.
   */
  public function isUsernameValid(&$username) {
    // @TODO: adjust, too
    // We want to allow "prename name <email@example.org>"
    // Or just "nick <email@example.org>"
    // Or just whatever naming convention you like
    // This means, we just check for control characters and NULs here
    if (preg_match("/[\\x00-\\x1f]/", $username) == 0) {
      return TRUE;
    }
    return FALSE;
  }

  /**
   * Overwrite
   */
  public function usernameSuggestion($user) {
    // @TODO: adjust later
    // Use the kindof standard git identifier here, it's nothing too serious.
    return $user->name ." <$user->mail>";
  }

}

class VersioncontrolGitOperation extends VersioncontrolOperation {
}
