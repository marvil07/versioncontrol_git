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
      'item' => 'VersioncontrolGitItem',
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

  /**
   * Implementation of abstract method.
   */
  public function getSelectedLabel($target_item) {
  // TODO: implement tag support here, tags>branch?
  // better not, after looking it again current code is OK.
  // just take the first branch, dunno what else we should do here...
  // jpetso knows neither :P
    return $this->labels[0];
  }

}

class VersioncontrolGitItem extends VersioncontrolItem {

  /**
   * Implementation abstract method.
   */
  public function getSelectedLabelFromItem(&$other_item, $other_item_tags = array()) {
    // First up, optimizations - maybe we can do without the generic
    // "label transfer" code from further down and use assumptions instead.
    // Let's assume for FakeVCS repositories that if an item wears a label, then
    // an item at another path but with the same (file-level) revision can also
    // wear that same label. That is the case with some version control systems
    // (e.g. Git, Mercurial, Bazaar) but might not be the case with others
    // (CVS for its lack of global revision identifiers, SVN for its use of
    // directory structure as tag/branch identifiers).
    if ($this->revision == $other_item->revision) {
      return $other_item->getSelectedLabel();
    }

    //can be maybe optimized for speed by using the hints provided
    return _versioncontrol_git_get_branch_intersect($this->repository, $this, $other_item);
  }

}
